{
  description = "haze: CUDA-shaped record-and-replay runtime for the Niobium FHE accelerator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # Pull vendor/niobium-fhetch (and its transitive openfhe / json
    # submodules) into self.outPath so the hermetic packages can read
    # source from `./vendor/...` paths.
    #
    # Caveat (nix issue #13324): with a clean parent worktree, nix
    # re-fetches submodules from their remotes rather than using the
    # local checkout — that needs auth for the private niobium-fhetch
    # remote. Keep a dirty file in the haze worktree to force local
    # use, or commit submodule bumps before invoking nix.
    self.submodules = true;
  };

  outputs =
    { self, nixpkgs }:
    let
      # x86_64-darwin omitted: niobium ships Apple Silicon only.
      systems = [
        "aarch64-linux"
        "x86_64-linux"
        "aarch64-darwin"
      ];
      forEachSystem = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Haze-owned .nix files for checks.fmt — vendor/ is excluded
      # since the submodule's formatting belongs to its upstream.
      hazeOwnedSrc =
        pkgs:
        pkgs.nix-gitignore.gitignoreSource [
          "vendor/"
          ".jj"
          ".direnv"
        ] ./.;

      # Dev shell + make-wrapping apps share this toolchain. clang-tools
      # is the unversioned package so clangd/clang-tidy track whatever
      # clang nixpkgs-unstable currently ships, matching clangStdenv
      # below.
      hazeTools =
        pkgs: with pkgs; [
          cmake
          catch2_3
          jujutsu
          clang-tools
          nixfmt
        ];

      # Three-derivation hermetic build: openfhe → niobium-fhetch → haze.
      # Each layer caches independently — haze edits don't reinvalidate
      # fhetch or openfhe. Uses clangStdenv (nixpkgs's default clang)
      # to match the dev shell and avoid the macOS SDK / ABI mismatch
      # trap.
      mkPackages =
        pkgs:
        let
          stdenv = pkgs.clangStdenv;
          fs = pkgs.lib.fileset;

          openfheSrc = ./vendor/niobium-fhetch/vendor/openfhe;

          # niobium-fhetch minus its OpenFHE submodule (separate
          # derivation) and any in-tree build dirs `make build` may
          # have written. maybeMissing tolerates a clean checkout
          # where those build dirs do not exist.
          fhetchSrc = fs.toSource {
            root = ./vendor/niobium-fhetch;
            fileset = fs.difference ./vendor/niobium-fhetch (
              fs.unions [
                ./vendor/niobium-fhetch/vendor/openfhe
                (fs.maybeMissing ./vendor/niobium-fhetch/vendor/lib)
                (fs.maybeMissing ./vendor/niobium-fhetch/build)
                (fs.maybeMissing ./vendor/niobium-fhetch/dbuild)
              ]
            );
          };

          hazeBuildSrc = fs.toSource {
            root = ./.;
            fileset = fs.unions [
              ./CMakeLists.txt
              ./include
              ./src
              ./test
              ./replay_bridge
            ];
          };

          openfhe = stdenv.mkDerivation {
            pname = "openfhe-niobium";
            version = "1.4.2";
            src = openfheSrc;
            nativeBuildInputs = [ pkgs.cmake ];
            # WITH_CPROBES=ON compiles in the niobium probe hooks that
            # libnbfhetch later links against. Other flags mirror the
            # Makefile's OPENFHE_CMAKE_FLAGS.
            cmakeFlags = [
              "-DBUILD_SHARED=ON"
              "-DBUILD_EXAMPLES=OFF"
              "-DBUILD_UNITTESTS=OFF"
              "-DBUILD_BENCHMARKS=OFF"
              "-DBUILD_EXTRAS=OFF"
              "-DWITH_CPROBES=ON"
              "-DWITH_OPENMP=OFF"
            ];
            meta.platforms = pkgs.lib.platforms.unix;
          };

          niobium-fhetch = stdenv.mkDerivation {
            pname = "niobium-fhetch";
            version = "1.0.0";
            src = fhetchSrc;
            nativeBuildInputs = [ pkgs.cmake ];
            buildInputs = [ openfhe ];
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DOPENFHE_INSTALL_DIR=${openfhe}"
              "-DJSON_INCLUDE_DIR=${fhetchSrc}/vendor/json/single_include"
            ];
            # TODO(niobium-fhetch): emit NiobiumFhetchConfig.cmake
            # upstream via configure_package_config_file. niobium-fhetch
            # currently installs only NiobiumFhetchTargets.cmake, so
            # find_package(NiobiumFhetch CONFIG) cannot resolve. Shim
            # the entry point here so consumers can use find_package.
            postInstall = ''
              cat > $out/lib/cmake/NiobiumFhetch/NiobiumFhetchConfig.cmake <<'EOF'
              include("''${CMAKE_CURRENT_LIST_DIR}/NiobiumFhetchTargets.cmake")
              EOF
            '';
            meta.platforms = pkgs.lib.platforms.unix;
          };

          # Shared cmake configure for the lint derivations below. They
          # only need compile_commands.json — no compile, no link, no
          # test. Same flags as the haze derivation so the database
          # matches what `make build` would emit. clang-tidy and clangd
          # walk up from each .cpp's path to discover .clang-tidy /
          # .clangd, which are copied into the source root in
          # postPatch since hazeBuildSrc deliberately omits them.
          lintCmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
            "-DHAZE_USE_PREBUILT_FHETCH=ON"
            "-DOPENFHE_INSTALL_DIR=${openfhe}"
            "-DJSON_INCLUDE_DIR=${fhetchSrc}/vendor/json/single_include"
          ];

          mkLintDerivation =
            { name, lintScript }:
            stdenv.mkDerivation {
              inherit name;
              src = hazeBuildSrc;
              nativeBuildInputs = [
                pkgs.cmake
                pkgs.clang-tools
              ];
              buildInputs = [
                openfhe
                niobium-fhetch
                pkgs.catch2_3
              ];
              cmakeFlags = lintCmakeFlags;
              postPatch = ''
                cp ${./.clang-tidy} .clang-tidy
                cp ${./.clangd} .clangd
              '';
              # cmake setup hook leaves us in build/. The lint walks
              # back to the source root so .clang-tidy / .clangd
              # discovery works for every .cpp.
              buildPhase = ''
                runHook preBuild
                cd ..
                ${lintScript}
                runHook postBuild
              '';
              dontUseCmakeInstall = true;
              installPhase = ''
                runHook preInstall
                touch $out
                runHook postInstall
              '';
            };

          haze = stdenv.mkDerivation {
            pname = "haze";
            version = "0.1.0";
            src = hazeBuildSrc;
            nativeBuildInputs = [ pkgs.cmake ];
            buildInputs = [
              openfhe
              niobium-fhetch
              pkgs.catch2_3
            ];
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DHAZE_USE_PREBUILT_FHETCH=ON"
              "-DOPENFHE_INSTALL_DIR=${openfhe}"
              # polynomial_io.cpp uses nlohmann/json. fhetch's
              # JSON_INCLUDE_DIR only propagates via add_subdirectory,
              # not via find_package; pass the same path here.
              "-DJSON_INCLUDE_DIR=${fhetchSrc}/vendor/json/single_include"
              # TODO(haze): add install() rules upstream. Without
              # them cmake never runs its build→install RPATH rewrite,
              # so the install RPATH must be baked at build time
              # (@loader_path on darwin, $ORIGIN on linux).
              "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON"
              "-DCMAKE_INSTALL_RPATH=${if stdenv.isDarwin then "@loader_path/../lib" else "$ORIGIN/../lib"}"
            ];
            # TODO(haze): add install() rules upstream. Until then the
            # cmake setup hook's `make install` would fail (no install
            # target), so skip it and place artifacts manually.
            dontUseCmakeInstall = true;
            installPhase = ''
              runHook preInstall
              mkdir -p $out/lib $out/bin $out/include
              # cp -a preserves any future SOVERSION/VERSION symlink
              # chains; install -t would flatten them to copies.
              cp -a libhaze.* replay_bridge/libhaze_replay_bridge.* $out/lib/
              cp -a haze_tests $out/bin/
              chmod +x $out/bin/haze_tests
              cp -r $src/include/haze $out/include/
              runHook postInstall
            '';
            meta.platforms = pkgs.lib.platforms.unix;
          };
          haze-clang-tidy = mkLintDerivation {
            name = "haze-clang-tidy";
            # `--warnings-as-errors='*'` overrides .clang-tidy's
            # narrower WarningsAsErrors: list, treating every enabled
            # check as an error. Headers are linted transitively via
            # the .cpp that includes them; only feed first-party .cpp
            # to clang-tidy.
            lintScript = ''
              find src replay_bridge test -name '*.cpp' -print0 \
                | xargs -0 -n4 -P"$NIX_BUILD_CORES" clang-tidy -p build \
                    --warnings-as-errors='*' --quiet
            '';
          };

          haze-clangd-check = mkLintDerivation {
            name = "haze-clangd-check";
            # `--check-locations=false` skips clangd's per-token
            # feature-test sweep (hover, ExtractFunction, etc.). Those
            # tweak tests print `E[...] tweak: ... FAIL` for break /
            # continue tokens in any function with a switch, which
            # makes clangd exit non-zero with no real diagnostic. The
            # flag does not filter parser, sema, or clang-tidy
            # diagnostics — they flow through both paths.
            #
            # Match warning / error diagnostic lines explicitly so
            # `.clangd`'s Diagnostics block (UnusedIncludes /
            # MissingIncludes: Strict) drives include hygiene through
            # the same gate. Capture into `report` (not `out`) since
            # `$out` is the nix output path and must not be
            # overwritten.
            lintScript = ''
              files=$(find src replay_bridge test -name '*.cpp')
              count=$(printf '%s\n' "$files" | grep -c .)
              echo "haze-clangd-check: linting $count first-party .cpp files"
              failed=0
              for f in $files; do
                report=$(clangd --check="$f" --check-locations=false 2>&1) || true
                if echo "$report" | grep -qE ': (warning|error):'; then
                  echo "=== $f ==="
                  echo "$report"
                  failed=1
                fi
              done
              test "$failed" = 0
            '';
          };
        in
        {
          inherit
            openfhe
            niobium-fhetch
            haze
            haze-clang-tidy
            haze-clangd-check
            ;
        };
    in
    {
      formatter = forEachSystem (pkgs: pkgs.nixfmt);

      devShells = forEachSystem (pkgs: {
        # Stdenv override sets the default clang (whatever nixpkgs
        # currently ships) as the auto-discovered cc/c++ for any tool
        # that probes the environment.
        default =
          (pkgs.mkShell.override {
            stdenv = pkgs.clangStdenv;
          })
            {
              name = "haze-dev";
              packages = hazeTools pkgs;
              shellHook = ''
                echo "haze dev shell ready (clang, cmake, catch2, jj, clang-tools)."
                echo "Run 'make sync && make build' to bootstrap, then 'make test'."
              '';
            };
      });

      # Each app re-enters the haze dev shell so catch2's cmake setup
      # hook is active (find_package(Catch2 3) needs it). Passing
      # `path:${self}` makes the shell load haze's flake regardless
      # of the caller's CWD — `make` itself still runs in CWD, which
      # must be a haze worktree.
      apps = forEachSystem (
        pkgs:
        let
          mkMakeApp = target: {
            type = "app";
            program = pkgs.lib.getExe (
              pkgs.writeShellApplication {
                name = "haze-${target}";
                runtimeInputs = [ pkgs.nix ];
                text = ''
                  exec nix develop "path:${self}" --command make ${target} "$@"
                '';
              }
            );
          };
        in
        {
          test-unit = mkMakeApp "test-unit";
          test-sim = mkMakeApp "test-sim";
          test = mkMakeApp "test";
          build = mkMakeApp "build";
        }
      );

      packages = forEachSystem (
        pkgs:
        let
          p = mkPackages pkgs;
        in
        {
          inherit (p) openfhe niobium-fhetch haze;
          default = p.haze;
        }
      );

      checks = forEachSystem (
        pkgs:
        let
          p = mkPackages pkgs;
        in
        {
          devshell-builds = self.devShells.${pkgs.stdenv.hostPlatform.system}.default;

          fmt =
            pkgs.runCommand "haze-nixfmt-check"
              {
                nativeBuildInputs = [
                  pkgs.nixfmt
                  pkgs.findutils
                ];
              }
              ''
                cd ${hazeOwnedSrc pkgs}
                find . -type f -name '*.nix' -print0 \
                  | xargs -0 -r nixfmt --check
                touch "$out"
              '';

          haze-build = p.haze;

          clang-tidy = p.haze-clang-tidy;

          clangd-check = p.haze-clangd-check;

          unit-tests = pkgs.runCommand "haze-unit-tests" { } ''
            mkdir -p $TMPDIR/runs && cd $TMPDIR/runs
            HAZE_TARGET=local ${p.haze}/bin/haze_tests "~[integration]"
            touch "$out"
          '';

          sim-tests = pkgs.runCommand "haze-sim-tests" { } ''
            mkdir -p $TMPDIR/runs && cd $TMPDIR/runs
            HAZE_TARGET=local ${p.haze}/bin/haze_tests "[integration]"
            touch "$out"
          '';
        }
      );
    };
}
