{
  description = "HAZE: API for Niobium FHE hardware";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [ "aarch64-linux" "x86_64-linux" "aarch64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in {
      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          llvm = pkgs.llvmPackages_19;
        in {
          default = pkgs.mkShell {
            name = "haze-dev";

            nativeBuildInputs = [
              llvm.clang
              pkgs.cmake
              pkgs.jujutsu
              pkgs.clang-tools # clang-format, clang-tidy
              pkgs.catch2_3
            ];

            # The haze repo's vendored niobium-compiler / niobium-fhetch
            # submodules carry in-tree build artefacts (build/, deps/...)
            # that the dev shell needs on CMAKE_PREFIX_PATH /
            # LD_LIBRARY_PATH. We resolve the worktree root at hook time
            # rather than baking ./vendor/... into the derivation: the
            # latter would coerce the path into a /nix/store/<hash>-source
            # snapshot (no in-tree build outputs) AND emit
            # `builtins.derivation … without a proper context` warnings
            # under recent nix.
            #
            # Resolution order: $HAZE_ROOT (caller override) -> the
            # closest enclosing flake.nix from $PWD. Errors loudly if
            # neither resolves so a misplaced `nix develop` invocation
            # fails fast instead of silently exporting empty paths.
            shellHook = ''
              if [ -n "''${HAZE_ROOT:-}" ]; then
                _haze_root="$HAZE_ROOT"
              else
                _haze_root="$PWD"
                while [ "$_haze_root" != "/" ] && [ ! -f "$_haze_root/flake.nix" ]; do
                  _haze_root="$(dirname "$_haze_root")"
                done
              fi
              if [ ! -f "$_haze_root/flake.nix" ]; then
                echo "haze devshell: cannot locate haze repo root from $PWD." >&2
                echo "haze devshell: set HAZE_ROOT or run 'nix develop' from inside the haze tree." >&2
                return 1
              fi

              _niobium_root="$_haze_root/vendor/niobium-compiler"
              _niobium_vendor="$_niobium_root/vendor/lib/niobium-compiler"
              _openfhe_vendor="$_niobium_root/vendor/lib/openfhe"
              _replay_lib="$_niobium_root/build"
              _photov_lib="$_niobium_root/deps/photovoltaic/dbuild"
              _ntl_lib="$_photov_lib/ntl/lib"

              export CMAKE_PREFIX_PATH="$_niobium_vendor''${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
              export LD_LIBRARY_PATH="$_openfhe_vendor/lib:$_replay_lib:$_photov_lib:$_ntl_lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
              # Re-export so subshells / scripts launched from the dev
              # shell skip the upward walk.
              export HAZE_ROOT="$_haze_root"
              unset _haze_root _niobium_root _niobium_vendor _openfhe_vendor _replay_lib _photov_lib _ntl_lib
              echo "haze dev shell ready (clang 19, cmake, jj)."
              echo "Build: cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel"
            '';
          };
        });
    };
}
