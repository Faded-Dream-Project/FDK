name: Bug report
description: Something crashes, renders wrong, or misbehaves
labels: ["bug"]
body:
  - type: markdown
    attributes:
      value: |
        Thanks for the report. The more of the fields below you can
        fill, the faster this can be acted on. FDK is a proprietary
        project in development — see CONTRIBUTING.md for what
        happens to reports.
  - type: input
    id: version
    attributes:
      label: FDK version string
      description: |
        From fdk_get_version_string() or the package version.
        A "-dev" suffix means an unreleased development build
        (docs/versioning.md explains the numbering).
      placeholder: "1.0.0-dev"
    validations:
      required: true
  - type: dropdown
    id: backend
    attributes:
      label: Backend
      options:
        - X11
        - Wayland
        - Both
        - Not display-related
    validations:
      required: true
  - type: input
    id: environment
    attributes:
      label: Environment
      description: Distro, WM/compositor (and version), architecture, GPU/driver if graphics-related.
      placeholder: "Debian 13, labwc 0.8, x86_64, llvmpipe"
    validations:
      required: true
  - type: textarea
    id: what-happened
    attributes:
      label: What happened
      description: A clear description of the wrong behavior, including any stderr output. FDK_LOG=debug raises verbosity.
    validations:
      required: true
  - type: textarea
    id: expected
    attributes:
      label: What you expected instead
    validations:
      required: true
  - type: textarea
    id: reproducer
    attributes:
      label: Minimal reproducer
      description: |
        A single .c file that builds against the installed FDK
        (gcc repro.c $(pkg-config --cflags --libs fdk)) is ideal.
        If the bug is in a file the toolkit parses (.fdk theme,
        .fmo catalog, .prefs, PNG/GIF/SVG), attach the file.
  - type: textarea
    id: notes
    attributes:
      label: Anything else
      description: Screenshots for visual bugs, log excerpts, guesses at the cause — or "nothing".
