# Control protocol

`sloppy-synth` serves the web UI over HTTP and speaks JSON over a WebSocket
at `/ws` on the same port (8080 by default). Every UI, including the web UI,
the Android app's WebView and a future encoder UI, uses this protocol.
One JSON object per text message, each with a `"type"`.

Parameter names are Vital's own (`filter_1_cutoff`, `macro_control_1`...),
the same keys `.vital` files use. Values are raw engine values in the
parameter's `min`..`max` range.

## Client to synth

| type | fields | reply |
| --- | --- | --- |
| `hello` / `get_state` | | `state` |
| `get_param_info` | | `param_info` |
| `list_patches` | | `patches` |
| `set` | `name`, `value` | none; clamped to range, then broadcast as `params` |
| `note` | `note` (0-127), `on` (default true), `velocity` (0-1), `channel` (1-16) | none |
| `all_notes_off` | | none |
| `load_patch` | `index` (from `patches`) | none on success (everyone gets `state`), else `error` |

## Synth to client

- `state`: `patch` (`index`, `name`, `author`, `style`, `comments`),
  `macros` (four macro names), `values` (every parameter). Sent on `hello`
  and to every client whenever a different patch is loaded.
- `params`: `values`, only the parameters that changed. Sent to every client
  about 25 times a second while anything is changing, whether the change came
  from a UI, MIDI or MIDI learn.
- `param_info`: `params`, a list of `name`, `label`, `min`, `max`, `default`,
  display hints (`scale`, `offset`, `multiply`, `invert`, `units`, see
  `displayValue` in `web/app.js`), and `options` for parameters with named
  values.
- `patches`: `current` and `patches`, a list of `index`, `name`, `bank`,
  `category`.
- `error`: `message`.

## Security

There is no authentication: anyone who can reach the port can play and edit
the synth. That suits a synth on a home network; use `--http-bind 127.0.0.1`
to keep it local, or `--no-web` to turn it off. The server only serves files
from the web folder, and never writes files.
