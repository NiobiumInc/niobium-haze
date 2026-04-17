{
  description = "HAZE: API for Niobium FHE hardware";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # TODO(task-06): niobium-compiler is not open-source but HAZE is intended
    # to be. Before publishing, swap this flake input for a pre-built, versioned
    # shared-object artifact (see how CUDA ships a closed runtime alongside
    # open-source consumers via stub libs + ICD loader for the pattern).
    niobium-compiler = {
      url = "git+file:///work/niobium-compiler";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, niobium-compiler }:
    let
      supportedSystems = [ "aarch64-linux" "x86_64-linux" "aarch64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          llvm = pkgs.llvmPackages_19;
          niobiumPkg = niobium-compiler.packages.${system}.default;
          openfhePkg = niobium-compiler.packages.${system}.openfhe;
        in {
          default = pkgs.stdenv.mkDerivation {
            pname = "libhaze";
            version = "0.1.0";

            # Filter untracked files out of the source so unrelated edits in
            # the worktree (build outputs, editor scratch) don't force a
            # Nix rebuild. See
            # https://ryantm.github.io/nixpkgs/functions/nix-gitignore
            src = pkgs.nix-gitignore.gitignoreSource [ ] ./.;

            nativeBuildInputs = [ pkgs.cmake llvm.clang ];
            buildInputs = [ pkgs.catch2_3 ];

            cmakeFlags = [ "-DCMAKE_PREFIX_PATH=${niobiumPkg}" ];

            # libnbcc depends on OpenFHE at link time; expose it to the linker.
            NIX_LDFLAGS = "-L${openfhePkg}/lib";

            preBuild = ''
              export LD_LIBRARY_PATH="${openfhePkg}/lib''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
            '';

            installPhase = ''
              runHook preInstall
              cmake --install . --prefix $out
              runHook postInstall
            '';
          };
        });

      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          llvm = pkgs.llvmPackages_19;

          # TODO: when we make the niobium compiler a submodule we should be able to remove these external paths.
          niobiumVendor = "/work/niobium-compiler/vendor/lib/niobium-compiler";
          openfheVendor = "/work/niobium-compiler/vendor/lib/openfhe";
          # Transitive runtime deps of libnbcc (not installed to vendor, built in-tree)
          replayLib = "/work/niobium-compiler/build";
          photovLib = "/work/niobium-compiler/deps/photovoltaic/dbuild";
          ntlLib = "/work/niobium-compiler/deps/photovoltaic/dbuild/ntl/lib";
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

            shellHook = ''
              export CMAKE_PREFIX_PATH="${niobiumVendor}''${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
              export LD_LIBRARY_PATH="${openfheVendor}/lib:${replayLib}:${photovLib}:${ntlLib}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
              echo "haze dev shell ready (clang 19, cmake, jj)."
              echo "Build: cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel"
            '';
          };
        });
    };
}
