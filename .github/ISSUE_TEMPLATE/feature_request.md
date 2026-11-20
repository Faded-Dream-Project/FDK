name: Feature request
description: A missing capability, widget, protocol, or API
labels: ["enhancement"]
body:
  - type: markdown
    attributes:
      value: |
        Requests that name a concrete user-facing outcome get
        acted on far faster than abstract ones ("a date picker
        widget", not "more widgets"). FDK is proprietary and in
        development — see CONTRIBUTING.md for the contribution
        posture.
  - type: textarea
    id: problem
    attributes:
      label: The problem you are trying to solve
      description: What you were building when the limitation appeared. This context matters more than the proposed solution.
    validations:
      required: true
  - type: textarea
    id: solution
    attributes:
      label: The capability you want
      description: What you wish existed — an API, a widget, a protocol binding, a behavior change.
    validations:
      required: true
  - type: textarea
    id: alternatives
    attributes:
      label: Alternatives you considered or tried
      description: Workarounds, other toolkits' answers to the same need, or "nothing works".
  - type: dropdown
    id: area
    attributes:
      label: Area
      options:
        - Widgets
        - Text / i18n / shaping
        - Rendering (CPU)
        - Rendering (GPU / GL)
        - X11 backend
        - Wayland backend
        - Desktop integration (D-Bus, portals, tray)
        - Accessibility
        - Theming
        - Build / packaging
        - Documentation
        - Other
