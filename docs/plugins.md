# Plugins

A plugin is a native shared library that FrameTee loads into its own process.
There is no sandbox: a running plugin has exactly the access the editor has, to
your files and to everything else you can reach. Enabling one is a decision to
run someone else's program, and the editor says so where that decision is made.

Two rules follow from that, and they shape everything below.

## A plugin that is off is never loaded

The editor loads a plugin only when it is enabled in `config.toml`. Nothing
about being listed, selected, searched or looked at loads it, because loading a
library runs its initializers before anything can be asked of it, there is no
way to peek inside without running it.

That is also why a plugin describes itself in a text file rather than through
an export. The editor has to be able to show a name for a plugin it will not
run, so the manifest is the only description there is, and what the editor knows
before you enable something is: the name of its directory, whatever its manifest
claims, and the checksum it computed itself.

## Layout

A plugin is a directory inside `plugins/`, holding its library, its manifest,
and anything else it needs:

```
plugins/
  physics_profiler/
    plugin.toml
    physics_profiler.dll        (libphysics_profiler.so on Linux)
    ...whatever resources it ships
```

The directory names the plugin. It is the same on every platform, unlike the
library file, which grows a `lib` prefix on one and not on the other, so the
directory name is what appears in the plugin list and what enables the plugin
in `config.toml`:

```toml
[plugins]
physics_profiler = true
```

The library inside carries that same name. Everything else in the directory
belongs to the plugin and is never mistaken for it, so a plugin may ship the
libraries it depends on beside itself; on Windows they are found there, and on
Linux an `$ORIGIN` rpath does the same.

A plugin's own directory is handed to it as `context->plugin_directory` for the
duration of `plugin_init`. Copy it if you need it later. That is where a plugin
should keep its resources, and the only place it can find them without guessing
where the editor was installed.

To distribute a plugin, zip that directory. The person installing it extracts
it into `plugins/`. The editor deliberately does not unpack archives itself: a
library has to be a real file on disk before it can be loaded, so accepting a
zip would mean extracting attacker-named files to a cache and loading from
there, and the file you checksummed would no longer be the file that runs.

## The manifest

`plugin.toml`, in the plugin's directory. The editor reads it as data. It is a
flat table of strings, all optional:

```toml
name = "Physics Profiler"
author = "Teero"
version = "1.0.0"
description = "Integrates Tracy to benchmark the ddnet_physics library."
game = "ddnet"
repository = "https://github.com/Teero888/frametee/tree/master/plugins/physics_profiler"
```

`game` mirrors the `plugin_game_id()` export and is shown as the plugin's scope
while it is unloaded. `repository` is offered as a link only when it is an
`http://` or `https://` address; anything else is shown as plain text, because
the platform call behind a link will open a great deal more than web pages.

None of it is verified, and none of it decides anything. The manifest sits in a
directory the plugin's author packed, so whoever writes one writes the library
next to it: it is the author's description of their own plugin, and the editor
shows it as a claim, marked as one. Its value is that you can read it, and
follow it to the source, *before* agreeing to run anything.

`game` is the exception that is only half a claim. It is what the plugin list
shows as the scope, but the editor acts on the `plugin_game_id()` export
instead, which cannot drift from the code the way a text file beside it can.

For plugins built in this repository, `plugin.toml` sits in the plugin's source
directory and the top-level build copies it into the built plugin's directory
and installs it into releases.

## The checksum

The editor hashes everything in a plugin's directory and shows the result in
the plugin's details, with a button to copy it. It covers the library, the
manifest and every resource, so it is one number for the whole plugin rather
than for one file of several. It is deliberately not read from the manifest, a
checksum that travels with the thing it describes only ever agrees with itself.

Compare it against what the author publishes for the release you meant to
install. A match means the directory on disk is the one they shipped.

The editor also remembers it. Enabling a plugin records its digest in
`config.toml` under `[plugin_checksums]`, and on every start the recorded digest
is compared against the directory as it is then. If they disagree, the plugin is
listed as `Changed` and is not loaded: you approved the files that were there
when you enabled it, not whatever replaced them. The details panel shows both
digests and offers `Enable this version`, which approves what is there now. That
is the expected step after you update a plugin yourself. If you did not update
it, it is worth finding out what did.

The recipe is one anybody can repeat without this editor. Hash every file in
the directory, then hash the `sha256sum` lines in ascending path order:

```sh
cd plugins/physics_profiler
find . -type f | sed 's|^\./||' | LC_ALL=C sort | xargs sha256sum | sha256sum
```

Paths are relative to the plugin directory and always use `/`, so the same
directory gives the same digest on Windows and Linux.

## Writing one

The exports the editor looks for are declared in `src/plugins/plugin_api.h`:
`plugin_abi_version`, `plugin_init`, `plugin_update`, `plugin_shutdown`, and the
optional `plugin_show_ui` and `plugin_game_id`. The bundled plugins under
`plugins/` are working examples in C and C++.

`plugin_abi_version` is required and there is a macro that writes it:

```c
FT_PLUGIN_ABI_EXPORT()
```

It returns `FRAMETEE_PLUGIN_ABI_VERSION`, which goes up whenever anything in
`plugin_api.h` changes shape. A plugin reporting a different number is refused
rather than loaded, because everything in that header is a layout two separately
compiled programs have to agree on, and nothing in the symbols themselves would
give away that they no longer do.

One constraint deserves its own note, because breaking it hangs the editor
rather than failing: **do not start or join threads while your library is
loading or unloading.** On Windows the loader lock is held across `DllMain`,
and a thread can neither start nor exit without it, so a library that waits for
a thread it started at load time deadlocks the editor before it can draw a
frame. Do that work in `plugin_init` and `plugin_shutdown`, which the editor
calls with no lock held. `plugins/physics_profiler` shows the pattern for a
library whose dependency wants to do exactly this.
