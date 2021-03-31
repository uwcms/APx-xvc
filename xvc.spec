# Build this using apx-rpmbuild.
%define name xvc

Name:           %{name}
Version:        %{version_rpm_spec_version}
Release:        %{version_rpm_spec_release}%{?dist}
Summary:        The APx XVC command line utility

License:        Reserved
URL:            https://github.com/uwcms/APx-%{name}
Source0:        %{name}-%{version_rpm_spec_version}.tar.gz

BuildRequires:  easymem easymem-devel ledmgr-devel
Requires:       easymem glibc
# We use the -l:libledmgr-dl.a facility, so depend on only glibc at runtime.

%global debug_package %{nil}

%description
This command line utility listens for XVC connections, and passes them to
hardware.


%prep
%setup -q


%build
##configure
make %{?_smp_mflags}


%install
rm -rf $RPM_BUILD_ROOT
install -D -m 0755 xvc %{buildroot}/%{_bindir}/xvc


%files
%{_bindir}/xvc


%changelog
* Fri Apr 10 2020 Jesra Tikalsky <jtikalsky@hep.wisc.edu>
- Initial spec file
