{
  description = "haze: CUDA-shaped record-and-replay runtime for the Niobium FHE accelerator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

    # External pin of niobium-fhetch (with its openfhe / json sub-submodules),
    # consumed only by the hermetic mkPackages derivations. `flake = false`
    # keeps it lazy so `nix develop` never resolves it (nix #13324).
    # The vendor/niobium-fhetch submodule stays the source of truth for
    # non-nix `make build`; CI gates that its rev matches the one pinned
    # here, and scripts/sync-fhetch-rev.sh realigns them after a bump.
    niobium-fhetch-src = {
      url = "git+https://github.com/NiobiumInc/niobium-fhetch.git?submodules=1";
      flake = false;
    };

    # Stock (unmodified) upstream OpenFHE, built SHARED for haze_e2e_tests'
    # reference crypto ONLY — never absorbed into libhaze.so. `?submodules=1`
    # pulls third-party/cereal (serialization), which the build needs.
    # `flake = false` keeps it lazy (dev shell never resolves it). The
    # vendor/openfhe submodule is the source of truth for non-nix `make`; CI
    # gates that its rev matches the one pinned here, and the daily openfhe-bump
    # workflow updates both together.
    openfhe-stock-src = {
      url = "git+https://github.com/openfheorg/openfhe-development.git?ref=refs/tags/v1.5.1&submodules=1";
      flake = false;
    };

    # ctcache (clang-tidy-cache): content-addressed clang-tidy wrapper that
    # returns the prior verdict for unchanged TUs; CI's clang-tidy gate uses
    # it. Pinned to a release; bump via `nix flake update --update-input
    # ctcache-src`.
    ctcache-src = {
      url = "github:matus-chochlik/ctcache/1.2.0";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      niobium-fhetch-src,
      openfhe-stock-src,
      ctcache-src,
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

      # Dev shell + make-wrapping apps share this toolchain. clang-tools is
      # unversioned so clangd/clang-tidy track nixpkgs's current clang,
      # matching clangStdenv. clang-tidy-cache is added per-system below (it
      # lives in mkPackages).
      hazeTools =
        pkgs: with pkgs; [
          cmake
          catch2_3
          jujutsu
          clang-tools
          nixfmt
        ];

      # Three-derivation hermetic build: openfhe → niobium-fhetch → haze, each
      # caching independently (haze edits don't reinvalidate fhetch/openfhe).
      # clangStdenv matches the dev shell and dodges the macOS SDK / ABI trap.
      mkPackages =
        pkgs:
        let
          stdenv = pkgs.clangStdenv;
          fs = pkgs.lib.fileset;

          # The -src suffix keeps the input from shadowing the
          # `niobium-fhetch` derivation below (let-bindings are mutually
          # recursive, so a collision infinite-loops).
          #
          # builtins.path with a filter is content-addressed: the hash comes
          # from surviving file contents, so an openfhe bump inside fhetch
          # (filtered out here) leaves fhetchSrc bit-identical and avoids
          # spurious rebuilds. fs.toSource can't — it needs a path-typed root,
          # but the input arrives as an attrset wrapping a store-path string.
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
              # Symbol-isolation version script, referenced at libhaze link time.
              ./linker
            ];
          };

          openfhe = stdenv.mkDerivation {
            pname = "openfhe-niobium";
            version = "1.4.2";
            src = openfheSrc;
            nativeBuildInputs = [ pkgs.cmake ];
            # WITH_CPROBES=ON compiles in the niobium probe hooks that
            # libnbfhetch later links against. Built static + PIC (not shared)
            # so it can be absorbed whole into the symbol-isolated libhaze.so.
            # Keep in sync with the canonical set in the Makefile's
            # OPENFHE_CMAKE_FLAGS (also mirrored in build-matrix.yml).
            cmakeFlags = [
              "-DBUILD_SHARED=OFF"
              "-DBUILD_STATIC=ON"
              "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
              "-DBUILD_EXAMPLES=OFF"
              "-DBUILD_UNITTESTS=OFF"
              "-DBUILD_BENCHMARKS=OFF"
              "-DBUILD_EXTRAS=OFF"
              "-DWITH_CPROBES=ON"
              "-DWITH_OPENMP=OFF"
              "-DWITH_REDUCED_NOISE=ON"
            ];
            meta.platforms = pkgs.lib.platforms.unix;
          };

          # Stock (unmodified) upstream OpenFHE for haze_e2e_tests' reference
          # crypto. Built SHARED (so it is never a candidate for libhaze's
          # *_static.a whole-archive absorb) and WITHOUT CPROBES (no niobium
          # instrumentation). WITH_REDUCED_NOISE=ON keeps its CKKS
          # keyswitch/ModDown math aligned with the recorder. Keep these flags
          # in sync with the Makefile's STOCK_OPENFHE_CMAKE_FLAGS.
          openfhe-stock = stdenv.mkDerivation {
            pname = "openfhe-stock";
            version = "1.5.1";
            src = openfhe-stock-src;
            nativeBuildInputs = [ pkgs.cmake ];
            cmakeFlags = [
              "-DBUILD_SHARED=ON"
              "-DBUILD_STATIC=OFF"
              "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
              "-DBUILD_EXAMPLES=OFF"
              "-DBUILD_UNITTESTS=OFF"
              "-DBUILD_BENCHMARKS=OFF"
              "-DBUILD_EXTRAS=OFF"
              "-DWITH_OPENMP=OFF"
              "-DWITH_REDUCED_NOISE=ON"
            ];
            meta.platforms = pkgs.lib.platforms.unix;
          };

          niobium-fhetch = stdenv.mkDerivation {
            pname = "niobium-fhetch";
            version = "1.0.0";
            src = fhetchSrc;
            nativeBuildInputs = [ pkgs.cmake ];
            buildInputs = [ openfhe ];
            # Static + link OpenFHE's *_static.a, so haze can absorb a static
            # libnbfhetch (matches the openfhe derivation above and the Makefile).
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DBUILD_SHARED_LIBS=OFF"
              "-DNIOBIUM_FHETCH_OPENFHE_STATIC=ON"
              "-DOPENFHE_INSTALL_DIR=${openfhe}"
              "-DJSON_INCLUDE_DIR=${fhetchSrc}/vendor/json/single_include"
            ];
            # TODO(niobium-fhetch): emit NiobiumFhetchConfig.cmake upstream.
            # It currently installs only NiobiumFhetchTargets.cmake, so
            # find_package(NiobiumFhetch CONFIG) can't resolve; shim it here.
            postInstall = ''
              cat > $out/lib/cmake/NiobiumFhetch/NiobiumFhetchConfig.cmake <<'EOF'
              include("''${CMAKE_CURRENT_LIST_DIR}/NiobiumFhetchTargets.cmake")
              EOF
            '';
            meta.platforms = pkgs.lib.platforms.unix;
          };

          # Shared cmake configure for the lint derivations: compile_commands
          # .json only, no compile/link/test. Same flags as haze so the
          # database matches `make build`. .clang-tidy is copied into the
          # source root in postPatch (hazeBuildSrc omits it) for clang-tidy's
          # upward discovery.
          lintCmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
            "-DHAZE_USE_PREBUILT_FHETCH=ON"
            "-DOPENFHE_INSTALL_DIR=${openfhe}"
            # Build the e2e suite + point it at the stock OpenFHE so its TUs land
            # in compile_commands.json with the right -isystem paths for clang-tidy.
            "-DHAZE_BUILD_E2E_TESTS=ON"
            "-DHAZE_TEST_OPENFHE_DIR=${openfhe-stock}"
            "-DJSON_INCLUDE_DIR=${fhetchSrc}/vendor/json/single_include"
          ];

          # ctcache packaged from the pinned input. Its entry point is a bash
          # script named `clang-tidy`; install it as `clang-tidy-cache` so it
          # doesn't shadow the real one. It auto-discovers src/ctcache/ via
          # dirname, so mirror the upstream layout under libexec/. makeWrapper
          # prepends python3 so its `/usr/bin/env python3` resolves to nix's.
          clang-tidy-cache = pkgs.stdenvNoCC.mkDerivation {
            pname = "clang-tidy-cache";
            version = "1.2.0";
            src = ctcache-src;
            nativeBuildInputs = [ pkgs.makeWrapper ];
            dontConfigure = true;
            dontBuild = true;
            installPhase = ''
              runHook preInstall
              mkdir -p $out/libexec/ctcache $out/bin
              cp clang-tidy $out/libexec/ctcache/clang-tidy-cache
              chmod +x $out/libexec/ctcache/clang-tidy-cache
              cp -r src $out/libexec/ctcache/
              makeWrapper $out/libexec/ctcache/clang-tidy-cache $out/bin/clang-tidy-cache \
                --prefix PATH : ${pkgs.python3}/bin
              runHook postInstall
            '';
            meta = {
              description = "Compilation database / caching wrapper for clang-tidy";
              homepage = "https://github.com/matus-chochlik/ctcache";
              license = pkgs.lib.licenses.boost;
              platforms = pkgs.lib.platforms.unix;
            };
          };

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
                openfhe-stock
                niobium-fhetch
                pkgs.catch2_3
              ];
              cmakeFlags = lintCmakeFlags;
              postPatch = ''
                cp ${./.clang-tidy} .clang-tidy
              '';
              # cmake setup hook leaves us in build/. The lint walks
              # back to the source root so .clang-tidy discovery works
              # for every .cpp.
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
              openfhe-stock
              niobium-fhetch
              pkgs.catch2_3
            ];
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DHAZE_USE_PREBUILT_FHETCH=ON"
              "-DOPENFHE_INSTALL_DIR=${openfhe}"
              # Build the black-box e2e suite against the stock OpenFHE (linked
              # SHARED into that exe only; never absorbed into libhaze.so).
              "-DHAZE_BUILD_E2E_TESTS=ON"
              "-DHAZE_TEST_OPENFHE_DIR=${openfhe-stock}"
              # polynomial_io.cpp uses nlohmann/json. fhetch's JSON_INCLUDE_DIR
              # only propagates via add_subdirectory, not find_package; pass it.
              "-DJSON_INCLUDE_DIR=${fhetchSrc}/vendor/json/single_include"
              # TODO(haze): add install() rules upstream. Without them cmake
              # skips its install RPATH rewrite, so bake the install RPATH at
              # build time (@loader_path on darwin, $ORIGIN on linux). The stock
              # OpenFHE store path is appended so haze_e2e_tests resolves its
              # libOPENFHE*.so at run time.
              "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON"
              "-DCMAKE_INSTALL_RPATH=${
                if stdenv.isDarwin then "@loader_path/../lib" else "$ORIGIN/../lib"
              };${openfhe-stock}/lib"
            ];
            # No install() rules upstream yet, so cmake's `make install` would
            # fail; skip it and place artifacts manually.
            dontUseCmakeInstall = true;
            installPhase = ''
              runHook preInstall
              mkdir -p $out/lib $out/bin $out/include
              # cp -a preserves any future SOVERSION/VERSION symlink chains;
              # install -t would flatten them to copies. The replay bridge is
              # now an OBJECT library absorbed into libhaze.so — no separate
              # libhaze_replay_bridge.* artifact to copy.
              cp -a libhaze.* $out/lib/
              cp -a haze_tests $out/bin/
              cp -a haze_e2e_tests $out/bin/
              chmod +x $out/bin/haze_tests $out/bin/haze_e2e_tests
              cp -r $src/include/haze $out/include/
              runHook postInstall
            '';
            meta.platforms = pkgs.lib.platforms.unix;
          };
          # Format-check source: .clang-format (read from the tree root) plus
          # the trees scripts/clang-format.sh walks. Separate from hazeBuildSrc
          # — the format check needs no CMakeLists or build dir.
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

          # Reproduces CI's clang-tidy findings under nix for local pre-push
          # (`nix build .#checks.<sys>.clang-tidy`). CI itself runs
          # scripts/clang-tidy.sh outside any derivation, through
          # clang-tidy-cache (see flake-check.yml). BUILD_DIR=build matches the
          # nixpkgs cmake setup hook — the `dbuild/` default only exists under
          # `make`, so without it the script can't find compile_commands.json.
          haze-clang-tidy = mkLintDerivation {
            name = "haze-clang-tidy";
            lintScript = "BUILD_DIR=build bash scripts/clang-tidy.sh";
          };

          # Configure-only derivation handing CI a hermetic
          # compile_commands.json without rebuilding openfhe / niobium-fhetch.
          # Two wrinkles:
          #
          #   1. The cc-wrapper injects -isystem flags at runtime; clang-tidy
          #      run outside a derivation (CI's ctcache path) never sees them.
          #      preConfigure bakes them into CMAKE_CXX_FLAGS so the database
          #      is portable.
          #   2. cmake bakes the /build/source sandbox root into the database;
          #      sed-rewrite it to the __HAZE_ROOT__ sentinel for the workflow
          #      to substitute. Linux-only path — widen the sed for macOS CI.
          haze-compile-commands = stdenv.mkDerivation {
            name = "haze-compile-commands";
            src = hazeBuildSrc;
            nativeBuildInputs = [ pkgs.cmake ];
            buildInputs = [
              openfhe
              openfhe-stock
              niobium-fhetch
              pkgs.catch2_3
            ];
            cmakeFlags = lintCmakeFlags;
            postPatch = ''
              cp ${./.clang-tidy} .clang-tidy
            '';
            preConfigure = ''
              # The cc-wrapper injects -isystem flags (libstdc++, glibc,
              # niobium-fhetch, ...) that cmake doesn't record. Ask clang for
              # its <...> search path, turn it into -isystem args, and bake
              # into CMAKE_CXX_FLAGS so out-of-sandbox libclang/clang-tidy
              # resolve <mutex> and niobium/compiler.h. awk splits on
              # whitespace — sound because nix store paths have no spaces.
              effective_flags=$(echo | clang++ -E -v -x c++ - 2>&1 \
                | sed -n '/^#include <\.\.\.>/,/^End of search/p' \
                | grep -E "^ /" \
                | awk '{print "-isystem " $1}' \
                | tr '\n' ' ')
              cmakeFlagsArray+=("-DCMAKE_CXX_FLAGS=$effective_flags")
            '';
            buildPhase = ''
              runHook preBuild
              runHook postBuild
            '';
            dontUseCmakeInstall = true;
            installPhase = ''
              runHook preInstall
              mkdir -p $out
              sed 's|/build/source|__HAZE_ROOT__|g' \
                compile_commands.json > $out/compile_commands.json
              runHook postInstall
            '';
          };
        in
        {
          inherit
            openfhe
            openfhe-stock
            niobium-fhetch
            haze
            haze-clang-format
            haze-clang-tidy
            haze-compile-commands
            clang-tidy-cache
            ;
        };
    in
    {
      formatter = forEachSystem (pkgs: pkgs.nixfmt);

      devShells = forEachSystem (
        pkgs:
        let
          p = mkPackages pkgs;
        in
        {
          # Stdenv override sets the default clang (whatever nixpkgs
          # currently ships) as the auto-discovered cc/c++ for any tool
          # that probes the environment.
          default =
            (pkgs.mkShell.override {
              stdenv = pkgs.clangStdenv;
            })
              {
                name = "haze-dev";
                # clang-tidy-cache comes from mkPackages so devs invoke
                # the same wrapper CI uses; set CTCACHE_CLANG_TIDY when
                # calling it so the wrapper finds the nix-pinned tidy.
                packages = (hazeTools pkgs) ++ [ p.clang-tidy-cache ];
                shellHook = ''
                  # Resolve haze worktree root via git so scripts/ stays
                  # on PATH even after `cd`-ing to a subdirectory. Falls
                  # back to PWD when invoked outside a git checkout.
                  haze_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
                  if [ -z "$haze_root" ]; then haze_root="$PWD"; fi
                  export PATH="$haze_root/scripts:$PATH"
                  echo "haze dev shell ready (clang, cmake, catch2, jj, clang-tools, clang-tidy-cache)."
                  echo "Run 'make sync && make build' to bootstrap, then 'make test'."
                '';
              };
        }
      );

      # Each app re-enters the haze dev shell so catch2's cmake setup hook is
      # active (find_package(Catch2 3) needs it). `path:${self}` loads haze's
      # flake regardless of CWD — `make` still runs in CWD, which must be a
      # haze worktree.
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
          inherit (p)
            openfhe
            openfhe-stock
            niobium-fhetch
            haze
            haze-compile-commands
            clang-tidy-cache
            ;
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

          # E2E suite: the public-C-ABI + stock-OpenFHE black-box exe (the
          # simple.cpp capstone + the interleave/no-clobber proof). Runs the
          # in-process FHETCH simulator inside libhaze.so via HAZE_TARGET=local.
          e2e-tests = pkgs.runCommand "haze-e2e-tests" { } ''
            mkdir -p $TMPDIR/runs && cd $TMPDIR/runs
            HAZE_TARGET=local ${p.haze}/bin/haze_e2e_tests
            touch "$out"
          '';

          # README examples: extract the fenced C + C++ examples from README.md,
          # compile each against the shipped libhaze (+ stock OpenFHE for the
          # C++ one), and run them through the in-process FHETCH simulator.
          # Docs-as-tests: the published snippets cannot drift without failing
          # CI. Points the script at the prebuilt store paths (no rebuild) and
          # runs in $TMPDIR so no recording artifacts land in the source root.
          readme-examples =
            pkgs.runCommand "haze-readme-examples"
              {
                nativeBuildInputs = [ pkgs.clang ];
                HAZE_LIB_DIR = "${p.haze}/lib";
                HAZE_INCLUDE_DIR = "${p.haze}/include";
                # replay_bridge.h is not installed into the haze package's
                # include dir; supply it straight from the source tree.
                HAZE_BRIDGE_INCLUDE_DIR = "${./replay_bridge/include}";
                STOCK_OPENFHE_DIR = "${p.openfhe-stock}";
                README = "${./README.md}";
                CC = "clang";
                CXX = "clang++";
              }
              ''
                export HAZE_RUNS_DIR="$TMPDIR/runs"
                bash ${./scripts/test_readme_examples.sh}
                touch "$out"
              '';

          # Isolation guard: assert the shipped libhaze exports ONLY the haze* C
          # ABI (no leaked OpenFHE symbols), the property that lets it coexist
          # with another OpenFHE in one process. A leak wouldn't fail the build,
          # so this check is what catches an isolation regression in CI.
          # Darwin needs llvm-nm (GNU binutils nm misreads Mach-O private-extern
          # symbols as global); ELF is fine with GNU nm.
          isolation =
            pkgs.runCommand "haze-isolation"
              {
                nativeBuildInputs = [ (if pkgs.stdenv.isDarwin then pkgs.llvm else pkgs.binutils) ];
              }
              ''
                set -euo pipefail
                bash ${./scripts/check_symbol_leak.sh} ${p.haze}/lib
                touch "$out"
              '';
        }
      );
    };
}
