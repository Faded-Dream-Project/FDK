# fdk-shell/packaging/fedora/fdk-shell.spec — RPM spec for fdk-shell.
#
# Built on AlmaLinux 10 (same container as FDK's fdk.spec) AFTER the
# FDK rpms are installed into the build environment: the spec's
# %build compiles against the INSTALLED public FDK (pkg-config) —
# the rpmbuild chroot carries ONLY the fdk-shell source tarball, so
# the isolation contract (installed headers/library only, no sibling
# FDK checkout anywhere on the compile line) is enforced by the
# build environment itself.
#
# @VERSION@ is substituted from fdk-shell's header (the packaging
# script); @MINVER@ — the FDK version that introduced extension API
# v1, in rpm's pre-release grammar — guards the versioned Requires.
#
# Subpackage layout:
#   libfdk-shell0   — runtime: libfdk-shell.so.0 (NEEDED: libfdk.so.1)
#   fdk-shell-devel — header + pkg-config metadata
#
# libfdk-shell.a is intentionally NOT packaged (FDK Proprietary
# License 4(d), same posture as FDK's own packages).

Name:           fdk-shell
Version:        @VERSION@
Release:        1%{?dist}
Summary:        Layer-shell surfaces (panels, docks, overlays) for FDK
License:        Proprietary
URL:            https://github.com/Faded-Dream-Project/FDK
Source0:        fdk-shell-%{version}.tar.gz
%define debug_package %{nil}
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconf-pkg-config
BuildRequires:  libX11-devel
BuildRequires:  libXrandr-devel
BuildRequires:  wayland-devel
# The installed FDK this package builds against (provided by the
# pipeline: fdk-devel's own Requires pull libfdk1 in).
BuildRequires:  fdk-devel

%description
fdk-shell is the first add-on package built on FDK's extension API
(fdk/extend.h): panel/bar/notification-style surfaces that live
outside the normal window stack — desktop background layers, docks,
top bars, overlays — behind one API on both FDK backends (Wayland
wlr-layer-shell v4; X11 dock windows with strut reservations and
sticky, always-on-top stacking hints).

Applications built with fdk-shell remain the property of their
authors; the FDK and fdk-shell runtimes are installed separately by
the end user per the FDK Proprietary License, Section 4(d).

%package -n libfdk-shell0
Summary:        Layer-shell surfaces for FDK — runtime library
# The REAL, versioned dependency on the FDK runtime: extension API v1
# first shipped in @MINVER@ (the documented add-on versioning floor).
# The ELF's NEEDED: libfdk.so.1 additionally produces the automatic
# soname dependency.
Requires:       libfdk1 >= @MINVER@

%description -n libfdk-shell0
The fdk-shell runtime: the shared library (libfdk-shell.so.0),
linked against the FDK runtime (libfdk.so.1) with a versioned
dependency on the FDK that introduced the extension API it needs.

%package devel
Summary:        Layer-shell surfaces for FDK — development files
Requires:       libfdk-shell0 = %{version}-%{release}
Requires:       fdk-devel >= @MINVER@
Requires:       pkgconf-pkg-config

%description devel
The fdk-shell public header and the pkg-config metadata (Requires:
fdk) for developing panels, docks and overlays against fdk-shell.
The static archive is deliberately NOT shipped (FDK Proprietary
License 4(d)).

%prep
%setup -q -n fdk-shell-%{version}

%build
# The ONLY FDK on the compile line is the installed one — the chroot
# has no FDK source tree (the isolation contract, by construction).
make CONFIG=release static shared \
    FDK_CFLAGS="$(pkg-config --cflags fdk)" \
    FDK_LIBS="$(pkg-config --libs fdk)" \
    FDK_SO_LIBS="$(pkg-config --libs fdk)"

%install
make CONFIG=release install DESTDIR=%{buildroot} PREFIX=%{_prefix} \
    LIBDIR=%{_libdir} INCDIR=%{_includedir} \
    FDK_CFLAGS="$(pkg-config --cflags fdk)" \
    FDK_LIBS="$(pkg-config --libs fdk)" \
    FDK_SO_LIBS="$(pkg-config --libs fdk)"
# The static archive is NOT shipped (license posture, see above).
rm -f %{buildroot}%{_libdir}/libfdk-shell.a

%post -n libfdk-shell0 -p /sbin/ldconfig
%postun -n libfdk-shell0 -p /sbin/ldconfig

%files -n libfdk-shell0
%license LICENSE
%doc THIRD-PARTY-NOTICES.md
%{_libdir}/libfdk-shell.so.0
%{_libdir}/libfdk-shell.so.0.*

%files devel
%license LICENSE
%{_includedir}/fdk-shell/
%{_libdir}/libfdk-shell.so
%{_libdir}/pkgconfig/fdk-shell.pc

%changelog
* Sun Oct 25 2026 FDK maintainers <https://github.com/Faded-Dream-Project/FDK> - @VERSION@-1
- First RPM packaging of fdk-shell (@VERSION@), built and verified by
  the upstream release pipeline against the installed FDK packages.
