{
  description = "haze: CUDA-shaped record-and-replay runtime for the Niobium FHE accelerator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # External pin of niobium-fhetch (with its own openfhe / json
    # sub-submodules). Consumed only by the hermetic mkPackages
    # derivations (openfhe, niobium-fhetch, haze and the lint
    # checks). `flake = false` keeps the input lazy: `nix develop`
    # never resolves it, so the dev shell stays free of the
    # SSH-auth dependency that `self.submodules = true` would
    # impose (nix issue #13324). Pointing at the upstream repo
    # rather than a `git+file:.` self-reference makes the lock
    # entry meaningful — the rev pins an external commit instead
    # of trying to embed haze's own HEAD inside itself.
    #
    # The submodule under vendor/niobium-fhetch remains the source
    # of truth for non-nix `make build` users; CI gates that the
    # submodule rev recorded in haze's index matches the rev pinned
    # in flake.lock here. `scripts/sync-fhetch-rev.sh` keeps them
    # aligned after a fhetch bump.
    niobium-fhetch-src = {
      url = "git+ssh://git@github.com/NiobiumInc/niobium-fhetch.git?submodules=1";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      niobium-fhetch-src,
    }:
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

          # niobium-fhetch-src input is the fhetch repo root (with
          # its vendor/openfhe and vendor/json sub-submodules embedded
          # via `?submodules=1`). The input is named -src so it does
          # not shadow the local `niobium-fhetch` derivation defined
          # below — let-bindings in nix are mutually recursive, so a
          # collision would cause infinite recursion.
          #
          # builtins.path with a filter produces a content-addressed
          # store path: the output hash is derived from the surviving
          # file contents, not from the input store path. So an
          # openfhe submodule bump inside fhetch leaves fhetchSrc
          # bit-identical (openfhe is filtered out), keeping the
          # niobium-fhetch derivation's input hash stable and
          # avoiding spurious rebuilds. fs.toSource cannot do this
          # because it requires a `path`-typed root, and the input
          # arrives as an attrset wrapping a /nix/store path string.
          fhetchSrc = builtins.path {
            name = "niobium-fhetch-src-filtered";
            path = niobium-fhetch-src;
            filter =
              path: _type:
              let
                rel = pkgs.lib.removePrefix "${niobium-fhetch-src}/" path;
              in
              !(pkgs.lib.hasPrefix "vendor/openfhe" rel);
          };
          openfheSrc = niobium-fhetch-src + "/vendor/openfhe";

          hazeBuildSrc = fs.toSource {
            root = ./.;
            fileset = fs.unions [
              ./CMakeLists.txt
              ./include
              ./scripts
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
          # Source for the format check: clang-format reads .clang-format
          # from the tree root, so we ship that plus the trees the script
          # walks (src/, include/, replay_bridge/, test/) and the script
          # itself. Kept separate from hazeBuildSrc because the format
          # check doesn't need CMakeLists / a configured build dir.
          hazeFmtSrc = fs.toSource {
            root = ./.;
            fileset = fs.unions [
              ./.clang-format
              ./scripts
              ./include
              ./src
              ./test
              ./replay_bridge
            ];
          };

          haze-clang-format =
            pkgs.runCommand "haze-clang-format"
              {
                nativeBuildInputs = [
                  pkgs.bash
                  pkgs.clang-tools
                  pkgs.findutils
                  pkgs.gnused
                ];
              }
              ''
                cd ${hazeFmtSrc}
                bash scripts/clang-format.sh --check
                touch "$out"
              '';

          # Both lint derivations dispatch through the scripts in
          # scripts/, which are also runnable directly from a haze
          # checkout (`scripts/clang-tidy.sh`, `scripts/clangd-check.sh`).
          # The derivations exist so `nix flake check` stays the CI gate
          # — they wrap each script in a hermetic toolchain (pinned
          # clang-tools, configured cmake build dir for
          # compile_commands.json) without duplicating lint logic.
          # BUILD_DIR=build matches what cmake's setup hook in nixpkgs
          # configures (the source-tree default `dbuild/` only exists
          # under `make`-driven builds). Without this, the scripts
          # bail with "no compile_commands.json under dbuild/".
          haze-clang-tidy = mkLintDerivation {
            name = "haze-clang-tidy";
            lintScript = "BUILD_DIR=build bash scripts/clang-tidy.sh";
          };

          haze-clangd-check = mkLintDerivation {
            name = "haze-clangd-check";
            lintScript = "BUILD_DIR=build bash scripts/clangd-check.sh";
          };
        in
        {
          inherit
            openfhe
            niobium-fhetch
            haze
            haze-clang-format
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
                # Resolve haze worktree root via git so scripts/ stays
                # on PATH even after `cd`-ing to a subdirectory. Falls
                # back to PWD when invoked outside a git checkout.
                haze_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
                if [ -z "$haze_root" ]; then haze_root="$PWD"; fi
                export PATH="$haze_root/scripts:$PATH"
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

          clang-format = p.haze-clang-format;

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
