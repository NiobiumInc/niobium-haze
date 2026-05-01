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
      # is taken from llvmPackages_19 so clangd/clang-tidy parse C++23
      # the same way the build's clang does.
      hazeTools =
        pkgs: with pkgs; [
          cmake
          catch2_3
          jujutsu
          llvmPackages_19.clang-tools
          nixfmt
        ];

      # Three-derivation hermetic build: openfhe → niobium-fhetch → haze.
      # Each layer caches independently — haze edits don't reinvalidate
      # fhetch or openfhe. Pinned to llvmPackages_19.stdenv to match the
      # dev shell and avoid the macOS SDK / ABI mismatch trap.
      mkPackages =
        pkgs:
        let
          stdenv = pkgs.llvmPackages_19.stdenv;
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
        in
        {
          inherit openfhe niobium-fhetch haze;
        };
    in
    {
      formatter = forEachSystem (pkgs: pkgs.nixfmt);

      devShells = forEachSystem (pkgs: {
        # Stdenv override pins clang-19 as the auto-discovered cc/c++
        # for any tool that probes the environment.
        default =
          (pkgs.mkShell.override {
            stdenv = pkgs.llvmPackages_19.stdenv;
          })
            {
              name = "haze-dev";
              packages = hazeTools pkgs;
              shellHook = ''
                echo "haze dev shell ready (clang 19, cmake, catch2, jj, clang-tools)."
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
