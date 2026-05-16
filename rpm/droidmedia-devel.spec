Name:          droidmedia-devel
Summary:       Android media wrapper library development package
Version:       0.20170214.0
Release:       1
License:       ASL 2.0
Source0:       %{name}-%{version}.tgz
BuildRequires: meson
BuildRequires: ninja
BuildRequires: photo-api-devel
BuildRequires: pkgconfig(egl)
Suggests:      libhybris
Suggests:      droidmedia = %{version}-%{release}

%description
%{summary}

%package -n photo-api-plugin-droid2
Summary:       Android Camera2 API plugin
Requires:      photo-api

%description -n photo-api-plugin-droid2
%{summary}

%prep
%setup -q

%build
%meson
meson rewrite kwargs set project / version %{version}
%meson_build

%install
%meson_install

%files
%defattr(-,root,root,-)
%{_libdir}/libdroidmedia.a
%{_includedir}/droidmedia/*.h
%{_datadir}/droidmedia/hybris.c
%{_libdir}/pkgconfig/droidmedia.pc

%files -n photo-api-plugin-droid2
%{_libdir}/photo-plugins/libphotoplugin-droid2.so
