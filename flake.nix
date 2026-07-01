{
  description = "LP-0017 Whistleblower — censorship-resistant document publishing on Logos";

  # Known issue (cold GUI build only): the zerokit (RLN) crate's vendored-deps
  # fixed-output derivation is fetched by nixpkgs' `fetch-cargo-vendor-util`,
  # which sends no User-Agent and can be 403'd by crates.io (a stock-nixpkgs bug
  # at the pinned rev). If you hit that on a clean `nix build .#whistleblower-install`,
  # supply a binary cache that already holds the `zerokit-*-vendor` path (via
  # `--extra-substituters`), or build from a machine where that FOD is already
  # materialised. This does NOT affect the terminal demo, the chronicle module,
  # or CI (which does not build the GUI); the GUI also ships as a prebuilt `.lgx`.

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    nixpkgs.follows           = "logos-module-builder/nixpkgs";

    nix-bundle-lgx.url = "github:logos-co/nix-bundle-lgx";

    storage_module.url = "github:logos-co/logos-storage-module";
    storage_module.inputs.logos-module-builder.follows = "logos-module-builder";

    delivery_module.url = "github:logos-co/logos-delivery-module";
    delivery_module.inputs.logos-module-builder.follows = "logos-module-builder";

    # Locked logoscore CLI for `nix run .#smoke-*` — no env setup needed.
    logoscore-cli.url = "github:logos-co/logos-logoscore-cli";
  };

  outputs = inputs@{ self, nixpkgs, logos-module-builder, ... }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];

      # The FFI shared library is built outside nix via `make ffi`.
      # If the file doesn't exist, nix eval gives a clear "no such file" message.
      ffiLib = ./logos-chronicle/vendored/libchronicle_registry_ffi.so;

      chronicleModule = logos-module-builder.lib.mkLogosModule {
        src        = ./logos-chronicle;
        configFile = ./logos-chronicle/metadata.json;
        flakeInputs = inputs;
        postInstall = ''
          mkdir -p $out/lib
          cp ${ffiLib} $out/lib/libchronicle_registry_ffi.so
        '';
      };

      whistleblowerModule = logos-module-builder.lib.mkLogosQmlModule {
        src        = ./logos-whistleblower;
        configFile = ./logos-whistleblower/metadata.json;
        flakeInputs = inputs // { chronicle = chronicleModule; };
      };

      # Per-system package sets, prefixed to avoid name collisions.
      packagesFor = system:
        let
          chrPkgs = chronicleModule.packages.${system}   or {};
          wbPkgs  = whistleblowerModule.packages.${system} or {};
          pfx = tag: set: nixpkgs.lib.mapAttrs'
            (k: v: nixpkgs.lib.nameValuePair "${tag}-${k}" v) set;
        in
        pfx "chronicle" chrPkgs // pfx "whistleblower" wbPkgs;

      # Smoke test apps: `nix run .#smoke-<name>` from the repo root.
      smokeAppsFor = system:
        let
          pkgs         = import nixpkgs { inherit system; };
          logoscoreBin = "${inputs.logoscore-cli.packages.${system}.cli}/bin/logoscore";
          storageMods  = "${inputs.storage_module.packages.${system}.install}/modules";
          deliveryMods = "${inputs.delivery_module.packages.${system}.install}/modules";
          chrMods      = "${chronicleModule.packages.${system}.install}/modules";

          mkSmoke = name: script: {
            type = "app";
            program =
              let
                wrapper = pkgs.writeShellApplication {
                  name            = "smoke-${name}";
                  runtimeInputs   = with pkgs; [ bash coreutils gnused procps python3 openssl ];
                  text = ''
                    [ -f "$PWD/integration-test.toml" ] || {
                      echo "smoke-${name}: run from the repo root (integration-test.toml not found in $PWD)" >&2
                      exit 1
                    }
                    export LOGOSCORE=${logoscoreBin}
                    export STORAGE_MODULES=${storageMods}
                    export DELIVERY_MODULES=${deliveryMods}
                    export CHRONICLE_MODULES=${chrMods}
                    export IT_REPO_ROOT="$PWD"
                    exec ${./logos-chronicle/scripts}/${script} "$@"
                  '';
                };
              in "${wrapper}/bin/smoke-${name}";
          };
        in {
          smoke-storage   = mkSmoke "storage"   "logoscore-storage-smoke.sh";
          smoke-broadcast = mkSmoke "broadcast" "logoscore-broadcast-smoke.sh";
          smoke-publish   = mkSmoke "publish"   "logoscore-publish-smoke.sh";
          smoke-anchor    = mkSmoke "anchor"    "logoscore-anchor-smoke.sh";
        };
    in {
      packages  = nixpkgs.lib.genAttrs supportedSystems packagesFor;
      apps      = nixpkgs.lib.genAttrs supportedSystems smokeAppsFor;
      devShells = chronicleModule.devShells or {};
      checks    = chronicleModule.checks    or {};
    };
}
