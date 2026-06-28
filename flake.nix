{
  description = "Whistleblower — LP-0017 censorship-resistant document upload + indexing pipeline for Logos";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    nixpkgs.follows = "logos-module-builder/nixpkgs";

    # Carried over from logos-chronicle/flake.nix.
    nix-bundle-lgx.url = "github:logos-co/nix-bundle-lgx";
    storage_module.url = "github:logos-co/logos-storage-module";
    storage_module.inputs.logos-module-builder.follows = "logos-module-builder";
    delivery_module.url = "github:logos-co/logos-delivery-module";
    delivery_module.inputs.logos-module-builder.follows = "logos-module-builder";

    # logoscore CLI — drives smoke tests against locked module revisions
    # so `nix run .#smoke-*` works from a fresh clone with zero env setup.
    logoscore-cli.url = "github:logos-co/logos-logoscore-cli";
  };

  outputs = inputs@{ self, nixpkgs, logos-module-builder, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];

      # Path to the pre-built FFI .so. Built outside nix via `make ffi` (which
      # runs `cargo build --release` in `ffi/` and stages the artifact here).
      # If this file doesn't exist, flake eval fails with "no such file" —
      # informative error pointing at the prereq.
      ffiSo = ./logos-chronicle/vendored/libchronicle_registry_ffi.so;

      chronicleMod = logos-module-builder.lib.mkLogosModule {
        src = ./logos-chronicle;
        configFile = ./logos-chronicle/metadata.json;
        flakeInputs = inputs;
        postInstall = ''
          mkdir -p $out/lib
          cp ${ffiSo} $out/lib/libchronicle_registry_ffi.so
          echo "chronicle: bundled FFI -> $out/lib/libchronicle_registry_ffi.so"
        '';
      };

      whistleblowerMod = logos-module-builder.lib.mkLogosQmlModule {
        src = ./logos-whistleblower;
        configFile = ./logos-whistleblower/metadata.json;
        flakeInputs = inputs // { chronicle = chronicleMod; };
      };

      # Flatten per-system packages, prefixed so they don't collide.
      packagesFor = system:
        let
          chronicleHere = chronicleMod.packages.${system} or {};
          whistleHere = whistleblowerMod.packages.${system} or {};
          prefix = pfx: set: nixpkgs.lib.mapAttrs'
            (n: v: nixpkgs.lib.nameValuePair "${pfx}-${n}" v) set;
        in
        prefix "chronicle" chronicleHere
        // prefix "whistleblower" whistleHere;

      # Smoke apps: `nix run .#smoke-<name>` runs the matching script in
      # logos-chronicle/scripts/ with module paths and logoscore resolved from
      # locked flake inputs. Cloners get zero-env-var-setup execution for
      # storage/broadcast/publish. anchor additionally sources
      # .scaffold/anchor.env (gitignored) for user-specific values.
      smokeAppsFor = system:
        let
          pkgs = import nixpkgs { inherit system; };
          logoscoreBin = "${inputs.logoscore-cli.packages.${system}.cli}/bin/logoscore";
          storageMods  = "${inputs.storage_module.packages.${system}.install}/modules";
          deliveryMods = "${inputs.delivery_module.packages.${system}.install}/modules";
          chronicleMods = "${chronicleMod.packages.${system}.install}/modules";

          mkSmokeApp = name: script:
            let
              # Smokes read all test-specific config (topic, sequencer URL,
              # wallet path, signer, program ID) from integration-test.toml
              # at the repo root; no extra env file needed.
              wrapper = pkgs.writeShellApplication {
                name = "smoke-${name}";
                runtimeInputs = [ pkgs.bash pkgs.coreutils pkgs.gnused pkgs.procps pkgs.python3 ];
                text = ''
                  export LOGOSCORE=${logoscoreBin}
                  export STORAGE_MODULES=${storageMods}
                  export DELIVERY_MODULES=${deliveryMods}
                  export CHRONICLE_MODULES=${chronicleMods}
                  # Scripts are copied into /nix/store when packaged, so the
                  # auto-walk in load-integration-config.sh can't find the
                  # repo. Caller must `nix run` from the repo root; we
                  # forward $PWD as the canonical root.
                  if [ ! -f "$PWD/integration-test.toml" ]; then
                    echo "smoke-${name}: integration-test.toml not found in \$PWD ($PWD)." >&2
                    echo "Run \`nix run .#smoke-${name}\` from the repo root." >&2
                    exit 1
                  fi
                  export IT_REPO_ROOT="$PWD"
                  exec ${./logos-chronicle/scripts}/${script} "$@"
                '';
              };
            in {
              type = "app";
              program = "${wrapper}/bin/smoke-${name}";
            };
        in {
          smoke-storage   = mkSmokeApp "storage"   "logoscore-storage-smoke.sh";
          smoke-broadcast = mkSmokeApp "broadcast" "logoscore-broadcast-smoke.sh";
          smoke-publish   = mkSmokeApp "publish"   "logoscore-publish-smoke.sh";
          smoke-anchor    = mkSmokeApp "anchor"    "logoscore-anchor-smoke.sh";
        };
    in
    {
      packages = nixpkgs.lib.genAttrs systems packagesFor;
      apps = nixpkgs.lib.genAttrs systems smokeAppsFor;

      devShells = chronicleMod.devShells or {};
      checks = chronicleMod.checks or {};
    };
}
