# packaging/fedora/fdk.spec — RPM spec for FDK.
#
# Built on AlmaLinux 10 (glibc 2.38) so the artifacts run on the
# RHEL 10 clone family AND on newer Fedora releases (glibc is
# forward-compatible). Install-tested on AlmaLinux 10 and Fedora 42
# by packaging/ci/fedora.sh.
#
# @VERSION@ is substituted from include/fdk/fdk_version.h by
# packaging/fedora/build-rpm.sh — the spec never carries a second,
# hand-edited version.
#
# Subpackage layout:
#   libfdk1   — runtime: libfdk.so.1 + the default theme (faded-dream)
#   fdk-devel — headers, dev symlink, pkgconfig, CLI tools
#
# libfdk.a is intentionally NOT packaged (FDK Proprietary License
# 4(d): distributing statically-linked FDK requires separate written
# authorization).

Name:           fdk
Version:        @VERSION@
Release:        1%{?dist}
Summary:        Faded Dream ToolKit — native C GUI toolkit for X11 and Wayland
License:        Proprietary
URL:            https://github.com/FemBoyGamerTechGuy/FDK
Source0:        fdk-%{version}.tar.gz
# The upstream release artifacts carry no debuginfo subpackages.
%define debug_package %{nil}
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconf-pkg-config
BuildRequires:  libX11-devel
BuildRequires:  libXext-devel
BuildRequires:  zlib-devel
BuildRequires:  wayland-devel
BuildRequires:  libxkbcommon-devel

%description
FDK is a general-purpose native C GUI toolkit for modern Linux
desktops: native X11 and Wayland backends talking directly to Xlib
and libwayland-client, a damage-tracked software rasterizer with an
optional GPU present lane, its own widget, layout, text, and theme
engines, and FDK-drawn window decorations.

Applications built with FDK remain the property of their authors;
the toolkit itself is proprietary and restrictive (see the LICENSE
shipped in this package). This package set is published by the
upstream maintainer — the distribution channel authorized under the
FDK Proprietary License, Section 4(d).

%package -n libfdk1
Summary:        Faded Dream ToolKit — runtime library and theme faces
# Runtime soname dependencies (libX11.so.6, libXext.so.6, libz.so.1,
# libwayland-client.so.0, libxkbcommon.so.0, libc/m) are generated
# automatically by rpmbuild from the ELF NEEDED entries.

%description -n libfdk1
The FDK runtime: the shared library (libfdk.so.1) and the default
theme (faded-dream; more faces are .FDKThemes drop-ins, never package
content). Applications built with FDK need this package
installed at run time; the FDK runtime is installed separately by
the end user per the license's separate-runtime model (Section 4(d)).

%package devel
Summary:        Faded Dream ToolKit — development files
Requires:       libfdk1 = %{version}-%{release}
Requires:       pkgconf-pkg-config

%description devel
Headers, the pkg-config metadata, the unversioned development
symlink, and the fdk-theme / fdk-set / fdk-prefs command-line tools
for developing applications against FDK.

Note: the static archive (libfdk.a) is deliberately NOT shipped;
distributing an application with FDK statically linked requires
separate written authorization under the FDK Proprietary License,
Section 4(d).

%prep
%setup -q -n fdk-%{version}

%build
# Plain make, not the Fedora make-build macro: the spec stays
# portable across rpm variants — Fedora, RHEL clones, and even a
# foreign rpmbuild for local validation. FDK_CONFIG is pinned here
# AND in the install section below: the
# Makefile's config stamp would otherwise silently rebuild the payload
# in debug config (sanitizers linked into the shipped .so) between the
# two stages.
make FDK_CONFIG=release all PREFIX=%{_prefix} LIBDIR=%{_libdir} \
    INCDIR=%{_includedir} BINDIR=%{_bindir} DATADIR=%{_datadir}

%install
make FDK_CONFIG=release DESTDIR=%{buildroot} PREFIX=%{_prefix} LIBDIR=%{_libdir} \
    INCDIR=%{_includedir} BINDIR=%{_bindir} DATADIR=%{_datadir} install
# The static archive is NOT shipped (license posture, see above).
rm -f %{buildroot}%{_libdir}/libfdk.a

%post -n libfdk1 -p /sbin/ldconfig
%postun -n libfdk1 -p /sbin/ldconfig

%files -n libfdk1
%license LICENSE
%doc THIRD-PARTY-NOTICES.md
%{_libdir}/libfdk.so.1
%{_libdir}/libfdk.so.1.*
%{_datadir}/fdk/

%files devel
%license LICENSE
%{_includedir}/fdk/
%{_libdir}/libfdk.so
%{_libdir}/pkgconfig/fdk.pc
%{_bindir}/fdk-theme
%{_bindir}/fdk-set
%{_bindir}/fdk-prefs

%changelog
* Sun Oct 11 2026 FDK maintainers <https://github.com/FemBoyGamerTechGuy/FDK> - @VERSION@-1
- First public RPM packaging of FDK (@VERSION@), built and verified
  by the upstream release pipeline (docs/packaging.md).
