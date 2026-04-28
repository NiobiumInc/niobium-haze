{
  description = "HAZE: API for Niobium FHE hardware";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    # niobium-compiler and niobium-fhetch are tracked as git submodules
    # under vendor/ (see .gitmodules). For Nix flake purity reasons we still
    # take the input via an absolute file URL today; when the repo is
    # published this becomes `git+file:./vendor/niobium-compiler` (or a
    # GitHub URL for niobium-compiler once it is open-sourced).
    niobium-compiler = {
      # Pin to main so we pick up the fast_base_convert CRT-constants fix
      # (#1206 / 1f78da4b). Earlier pins on fix branches predate that fix.
      url = "git+file:///work/niobium-compiler?ref=main";
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

          # Paths inside the niobium-compiler submodule. These pick up the
          # in-tree build artifacts produced by `make build` /
          # `make build-release` over in vendor/niobium-compiler.
          niobiumRoot = toString ./vendor/niobium-compiler;
          niobiumVendor = "${niobiumRoot}/vendor/lib/niobium-compiler";
          openfheVendor = "${niobiumRoot}/vendor/lib/openfhe";
          # Transitive runtime deps of libnbcc (not installed to vendor, built in-tree)
          replayLib = "${niobiumRoot}/build";
          photovLib = "${niobiumRoot}/deps/photovoltaic/dbuild";
          ntlLib = "${niobiumRoot}/deps/photovoltaic/dbuild/ntl/lib";
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
