# sw2-store

The homebrew store for Switch 2 launchers built on [sw2-sdk](https://github.com/sw2-homebrew-dev/sw2-sdk).
It installs apps from [sw2-homebrew-catalog](https://github.com/sw2-homebrew-dev/sw2-homebrew-catalog).

| | |
| --- | --- |
| **On console** (`app/`) | A native app that appears in the launcher. Browse the catalog, then install, update or remove apps from the couch. |
| **Desktop fallback** (`sw2-store`) | The same catalog, installer and `apps/` folder, from a terminal on the PC. |

## How it works

On this platform the PC hosts the app library, and the console runs whatever
the host maps into it. The store app can't download anything by itself.
Instead it uses the sw2-sdk **services mailbox**:

```
store (console)                      sw2-sdk host (PC)
  pick "Install foo" ──HBX_SVC_CALL──▶ download artifact_url
                                       verify sha256 from the catalog
                                       unpack + validate the package
                                       swap into apps/foo
  "installed Foo 1.2" ◀──re-enter──── reply + refreshed catalog page
```

When the store exits, the launcher's list is rescanned, so new apps show up
right away. Only apps that the store installed can be updated or removed.
Apps you are developing in `apps/` are never touched.

## Install

```sh
SW2_SDK=../sw2-sdk ./install.sh     # builds the app into sw2-sdk/apps/sw2-store
../sw2-sdk/sw2 run                  # or: sw2 run --mock
```

Controls: **D-pad/stick** move · **L/R** page · **A** install/update ·
**X** twice to remove · **Y** refresh from the network · **B** back.

## Desktop

```sh
./sw2-store list                 # everything, with installed/update state
./sw2-store list --updates
./sw2-store install <id> [...]
./sw2-store update               # every store-installed app with an update
./sw2-store remove <id>
./sw2-store refresh
```

## Choosing a catalog

Both clients read `SW2_CATALOG_URL`. It defaults to the catalog's GitHub
Pages `index.json`. To use a fork or a local build:

```sh
export SW2_CATALOG_URL=https://sw2-homebrew-dev.github.io/sw2-homebrew-catalog/index.json
export SW2_CATALOG_URL=file:///path/to/_site/index.json
```

## Tests

`sh tests/run.sh` compiles the store for the workstation with stubbed
runtime calls. It then plays the host's side of every mailbox round trip:
load, install, reload, the two-step remove, errors, refresh and exit.

## License

MIT
