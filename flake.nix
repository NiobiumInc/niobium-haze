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

    # ctcache (clang-tidy-cache) — wraps clang-tidy with a content-
    # addressed cache so unchanged TUs return their prior verdict
    # without re-running checks. CI uses this for the clang-tidy
    # gate; the cache directory persists across runs via
    # actions/cache. Pinned to a tagged release for reproducibility
    # — bump via `nix flake update --update-input ctcache-src`.
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

      # Dev shell + make-wrapping apps share this toolchain. clang-tools
      # is the unversioned package so clangd/clang-tidy track whatever
      # clang nixpkgs-unstable currently ships, matching clangStdenv
      # below. clang-tidy-cache is added via the per-system `extra`
      # arg below so devs get the same cache wrapper CI uses; passed
      # in rather than referenced here because it lives in mkPackages.
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

          # Shared cmake configure for the lint derivations below.
          # They only need compile_commands.json — no compile, no link,
          # no test. Same flags as the haze derivation so the database
          # matches what `make build` would emit. clang-tidy walks up
          # from each .cpp's path to discover .clang-tidy, which is
          # copied into the source root in postPatch since hazeBuildSrc
          # deliberately omits it.
          lintCmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
            "-DHAZE_USE_PREBUILT_FHETCH=ON"
            "-DOPENFHE_INSTALL_DIR=${openfhe}"
            "-DJSON_INCLUDE_DIR=${fhetchSrc}/vendor/json/single_include"
          ];

          # ctcache (https://github.com/matus-chochlik/ctcache) packaged
          # from the pinned flake input. The upstream entry point is
          # a bash script named `clang-tidy` (intended to shadow the
          # real one on PATH); we install it under the explicit
          # `clang-tidy-cache` name so it can be invoked without
          # shadowing. The script auto-discovers `src/ctcache/` via
          # `dirname $(realpath $0)`, so the layout under libexec/
          # mirrors the upstream tree. makeWrapper prepends python3
          # to PATH so the script's `/usr/bin/env python3` lookup
          # always resolves to the nix-pinned interpreter.
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

          # The clang-tidy lint derivation dispatches through
          # `scripts/clang-tidy.sh`, which is also runnable directly from
          # a haze checkout. The derivation exists so contributors can
          # reproduce CI's lint findings byte-for-byte under nix —
          # `nix build .#checks.<sys>.clang-tidy` is the local pre-push
          # check. CI itself runs scripts/clang-tidy.sh outside of any
          # derivation so it can pipe through clang-tidy-cache with a
          # persistent actions/cache; see flake-check.yml. BUILD_DIR=build
          # matches what cmake's setup hook in nixpkgs configures (the
          # source-tree default `dbuild/` only exists under `make`-driven
          # builds). Without this, the script bails with "no
          # compile_commands.json under dbuild/".
          haze-clang-tidy = mkLintDerivation {
            name = "haze-clang-tidy";
            lintScript = "BUILD_DIR=build bash scripts/clang-tidy.sh";
          };

          # Configure-only derivation that hands CI a hermetic,
          # self-contained compile_commands.json without rebuilding
          # openfhe / niobium-fhetch (those are cached via Magic Nix
          # Cache). Mirrors mkLintDerivation's cmake setup but runs no
          # lint in buildPhase.
          #
          # Two wrinkles addressed in preConfigure / installPhase:
          #
          #   1. NIX_CFLAGS_COMPILE injection. The cc wrapper applies
          #      NIX_CFLAGS_COMPILE (-isystem paths for openfhe,
          #      niobium-fhetch, the C++ stdlib, etc.) at runtime when
          #      it dispatches to the real clang. That works inside a
          #      derivation because the lint runs in the same shell.
          #      Outside the derivation (e.g., CI's actions/cache step
          #      that runs scripts/clang-tidy.sh through the ctcache
          #      wrapper), clang-tidy invokes libclang directly — no
          #      wrapper, no NIX_CFLAGS_COMPILE. Baking the flags into
          #      CMAKE_CXX_FLAGS makes them part of every recorded
          #      compile command, so the database is portable.
          #
          #   2. /build/source rewrite. cmake bakes the sandbox source
          #      root into `directory`, `file`, and `-I` flags. We
          #      sed-rewrite to the __HAZE_ROOT__ sentinel; the
          #      workflow then substitutes its workspace path at
          #      consume time. /nix/store paths survive untouched
          #      because they don't share the /build/source prefix.
          #      CI's runner is Linux (ubuntu-latest); on macOS nix
          #      uses a different sandbox path (/private/var/...).
          #      If we ever build this derivation on macOS in CI the
          #      sed pattern needs to be widened.
          haze-compile-commands = stdenv.mkDerivation {
            name = "haze-compile-commands";
            src = hazeBuildSrc;
            nativeBuildInputs = [ pkgs.cmake ];
            buildInputs = [
              openfhe
              niobium-fhetch
              pkgs.catch2_3
            ];
            cmakeFlags = lintCmakeFlags;
            postPatch = ''
              cp ${./.clang-tidy} .clang-tidy
            '';
            preConfigure = ''
              # The cc-wrapper injects -isystem and -cxx-isystem flags
              # at compile time (for libstdc++, glibc, niobium-fhetch's
              # built derivation, etc.); cmake doesn't capture those
              # in compile_commands.json. Ask clang itself for the
              # full <...> search path and turn it into -isystem args,
              # then bake them into CMAKE_CXX_FLAGS so libclang/
              # clang-tidy consumers outside this sandbox resolve
              # <mutex> and niobium/compiler.h without the wrapper.
              #
              # Relies on the nix-store-paths-have-no-spaces invariant
              # — awk splits on whitespace, which would corrupt any
              # path containing one. Nix's store hashing forbids spaces
              # in derivation outputs, so this is sound today.
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
          inherit (p)
            openfhe
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
        }
      );
    };
}
