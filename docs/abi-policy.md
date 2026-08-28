# FDK ABI Policy

## Current status: ABI-stable (1.0)

`FDK_ABI_STABLE` (in `include/fdk/fdk_version.h`) is `1` as of the
Phase 11 stabilization pass. The 0.x series is over: Phases 1–10
built the widget/layout/text/theme/i18n/a11y surfaces against real
usage, and the Phase 11 audit (below) classified every public struct
and verified the freeze rules against the actual headers. Between
1.x releases the rules in this document now BIND.

### The struct classification (the Phase 11 audit)

Every struct in a public header falls into exactly one class, and
each class has its own change rule:

| Class | Structs | Rule after 1.0 |
|---|---|---|
| **Opaque objects** | `fdk_context`, `fdk_window`, `fdk_widget`, `fdk_font`, `fdk_surface`, `fdk_theme`, `fdk_catalog`, `fdk_menu`, `fdk_menu_item`, `fdk_dialog` | Layout lives under `src/`; NEVER exposed. Free to change internally. |
| **Value types** | `fdk_point`, `fdk_size`, `fdk_rect`, `fdk_pointf`, `fdk_sizef`, `fdk_rectf`, `fdk_color`, `fdk_date`, `fdk_time`, `fdk_locale`, `fdk_plural_operands`, `fdk_widget_event`, `fdk_a11y_event`, the `fdk_event_data` union and its members, `fdk_font_metrics`, `fdk_text_metrics`, `fdk_font_cache_stats`, `fdk_surface_info`, `fdk_text_line` | FROZEN: no field changes, no reordering, no size change. Passed by value freely (they are FDK's `GdkRectangle`/`QRect`). The library carries compile-time size assertions (`src/core/abi_check.c`) so an accidental edit fails the build, not someone's app. |
| **Input structs** | `fdk_init_options`, `fdk_window_options`, `fdk_dialog_options`, `fdk_number_options` | Append-only, zero-init-safe, passed by pointer (documented at each definition). |
| **Result structs** | `fdk_a11y_info` | CALLER-allocated and library-filled (with owned strings released via `fdk_a11y_info_free`): treated as frozen — future additions get ACCESSOR functions, never new fields, because an old caller's allocation is smaller than a new library's writes. |
| **Vtables** | `fdk_widget_class`, `fdk_a11y_class` | Append-only (new hooks at the end, NULL = absent). Every in-tree definition uses designated initializers, so appending is source-compatible for subclass authors too. |

Enums: values may be APPENDED (never renumbered, never removed) —
the existing rule, unchanged.

### The subclassing decision at 1.0

`fdk_widget_class` (the hook vtable) is public and always was: it
documents the hook contracts the layout engine drives, and internal
widget types fill it. What stays INTERNAL is the `struct fdk_widget`
LAYOUT — embedding `fdk_widget` as a first member requires the
layout, and exposing it would freeze the object model's field order,
size, and every internal invariant forever (the GTK-vs-Qt lesson:
GTK4's move to Gadgets happened precisely because GtkObject's public
layout was unmaintainable).

The 1.0 extension surface is deliberate and sufficient:

- **class hooks + user_data**: an application may define its own
  static `fdk_widget_class` (paint/measure/arrange/event/destroy
  hooks, per-instance state in `user_data` via a side table or an
  embedded-after-the-fact allocation) and create widgets with it
  through `fdk_widget_create`;
- **event callbacks**: `fdk_widget_set_event_callback` covers
  reactive behavior without any class;
- **Canvas**: custom drawing with the full surface primitive set;
- **composition**: the catalog's 20+ widget types plus boxes/grids.

If real applications hit the wall where per-instance fields in C
require layout exposure, the 2.0 answer is an allocation-callback
pattern (the library allocates `klass->size` bytes the subclass
requests), not layout exposure — recorded here so the decision has
its paper trail.

## What "ABI-safe" means for FDK

The following rules apply and are enforced by review:

1. **All public objects are opaque.** Every type in `fdk_types.h`
   (`fdk_context`, `fdk_window`, `fdk_widget`, etc.) is a forward
   declaration only; applications never see or depend on struct
   layout. This is already true in Phase 1 and is not expected to
   change — it's the mechanism that makes the rest of this policy
   possible.
2. **No struct in a public header may change size or field order**
   once shipped in a stable release, *except* structs explicitly
   documented as "input structs" (like `fdk_init_options`) which:
   - are always passed by pointer, never by value, to API functions
   - are always safe to zero-initialize for defaults
   - may only ever have fields **appended**, never removed or
     reordered, and only with a documented default equivalent to
     zero/NULL
3. **No public function signature changes.** A behavior change that
   needs new parameters gets a new function name (e.g.
   `fdk_window_create_ex`), not a modified existing signature.
4. **Enums may gain new values** (appended, not renumbered) but
   existing values' numeric meaning never changes.
5. **Removing a public symbol is a major-version-bump event**, not a
   minor one.

## Versioning

`fdk_get_version()` / `fdk_get_version_string()` let an application
detect a mismatch between the headers it compiled against and the
`.so` it loaded at runtime — always check these if you're loading FDK
as a plugin/dlopen rather than linking it normally.

## Practical guidance for FDK's own implementers (pre-1.0)

Even though ABI isn't guaranteed yet, treat the public headers as if
it mattered, because:
- it forces the opaque-pointer discipline that makes stability
  *possible* later
- it avoids accumulating a pile of ABI debt as the surface grows

Internal-only structs (anything under `src/`, e.g.
`context_internal.h`) have no such constraint and may change freely —
that's the whole point of keeping them out of `include/`.
