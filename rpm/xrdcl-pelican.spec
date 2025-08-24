
Name: xrdcl-pelican
Version: 1.5.1
Release: 1%{?dist}
Summary: A Pelican-specific backend for the XRootD client

Group: System Environment/Daemons
License: BSD
URL: https://github.com/pelicanplatform/xrdcl-pelican
# Generated from:
# git archive v%%{version} --prefix=xrdcl-pelican-%%{version}/ | gzip -7 > ~/rpmbuild/SOURCES/xrdcl-pelican-%%{version}.tar.gz
Source0: %{name}-%{version}.tar.gz
Source1: tinyxml2-10.0.0.tar.gz

%define xrootd_current_major 5
%define xrootd_current_minor 6
%define xrootd_next_major 6

%if 0%{?rhel} > 8
%global __cmake_in_source_build 1
%endif

BuildRequires: xrootd-devel >= 1:%{xrootd_current_major}
BuildRequires: xrootd-devel <  1:%{xrootd_next_major}
BuildRequires: xrootd-client-devel >= 1:%{xrootd_current_major}
BuildRequires: xrootd-client-devel <  1:%{xrootd_next_major}
BuildRequires: xrootd-server-devel >= 1:%{xrootd_current_major}
BuildRequires: xrootd-server-devel <  1:%{xrootd_next_major}
%if 0%{?rhel} > 8
BuildRequires: gcc-c++
BuildRequires: cmake
%else
BuildRequires: cmake3
%endif
%if 0%{?rhel} == 7
BuildRequires: devtoolset-11-toolchain
%endif
%if 0%{?rhel} == 8
BuildRequires: gcc-toolset-11-toolchain
# Turn off annobin: it does not work with gcc-toolset without a hackaround that we don't have permissions to do:
#   https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/9/html/developing_c_and_cpp_applications_in_rhel_9/assembly_additional-toolsets-for-development-rhel-9_developing-applications#ref_specifics-of-annobin-in-gcc-toolset-12_gcc-toolset-12
%undefine _annotated_build
%endif
BuildRequires: curl-devel
%{?systemd_requires}
# For %%{_unitdir} macro
BuildRequires: systemd
BuildRequires: openssl-devel
# nlohmann-json-devel is available from the OSG repos
BuildRequires: nlohmann-json-devel
%if 0%{?rhel} >= 9
BuildRequires: tinyxml2-devel >= 9
%endif

Requires: xrootd-client >= 1:%{xrootd_current_major}.%{xrootd_current_minor}
Requires: xrootd-client <  1:%{xrootd_next_major}.0.0-1

%description
%{summary}

%prep
%setup -q

%build
%if 0%{?rhel} == 7
. /opt/rh/devtoolset-11/enable
%endif
%if 0%{?rhel} == 8
. /opt/rh/gcc-toolset-11/enable
%endif

%if 0%{?rhel} >= 9
%cmake3 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXROOTD_EXTERNAL_TINYXML2=1 -DXROOTD_EXTERNAL_JSON=1 -DXrdClCurl_VERSION_STRING=%{version} .
%else
cp %{SOURCE1} cmake/tinyxml2/
%cmake3 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXROOTD_EXTERNAL_JSON=1 -DXrdClCurl_VERSION_STRING=%{version} .
%endif
make VERBOSE=1 %{?_smp_mflags}

%install
make install DESTDIR=$RPM_BUILD_ROOT

%files
%{_libdir}/libXrdClCurl-*.so
%{_libdir}/libXrdClPelican-*.so
%{_libdir}/libXrdClS3-*.so
%{_sysconfdir}/xrootd/client.plugins.d/curl-plugin.conf
%{_sysconfdir}/xrootd/client.plugins.d/pelican-plugin.conf
%{_sysconfdir}/xrootd/client.plugins.d/s3-plugin.conf

%changelog
* Tue Aug 12 2025 Mátyás Selmeci <mselmeci@wisc.edu> 1.5.1-1
- Add extensive statistics about the performance of the XrdClCurl client

* Tue Aug 12 2025 Mátyás Selmeci <mselmeci@wisc.edu> 1.4.2-1
- Fix deadlock due to incorrect locking order

* Sat Jul 19 2025 Brian Bockelman <bbockelman@morgridge.org> 1.4.0-1
- Add experimental support for s3://-style URLs.
- Delay initialization of thread pools until the first file is opened.
- Cleanly shutdown helper threads when plugin is unloaded, reducing
  the chance of a segfault when a CLI or unit test is shutting down.
- Fix a few minor memory leaks and race conditions uncovered by
  AddressSanitizer.

* Tue Jun 17 2025 Brian Bockelman <bbockelman@morgridge.org> 1.3.1-1
- Fix minor build issues picked up by the EL8/9 compilers.

* Sat Jun 14 2025 Brian Bockelman <bbockelman@morgridge.org> 1.3.0-1
- Split code in half, creating a pure-HTTP(S) plugin which only invokes
  libcurl and a Pelican-only plugin that depends on the HTTP one.
- Add support for delete operations.
- Add support for creating directories.
- Add support for cache-control and Etag queries.
- Implement a prefetch scheme, removing the need for round trips for
  each operation if you are simply reading the whole file.

* Sat Apr 26 2025 Brian Bockelman <bbockelman@morgridge.org> 1.2.1-1
- Fix invalid memory read on handle reuse after a checksum operation.

* Wed Apr 16 2025 Matt Westphall <westphall@wisc.edu> - 1.2.0-2
- Rebuild against XRootD 5.7.3

* Sat Apr 12 2025 Brian Bockelman <bbockelman@morgridge.org> - 1.2.0-1
- Add progress- and stall-based timeout checks.
- Add timeout check while requests are queued.
- Increase the default number of workers to 8.
- Add ability to tune more parameters via environment variables.
- Fix bug where uploads fail for files that don't exist
- Fix thread-safety issues which could cause a libcurl deadlock.

* Sat Apr 5 2025 Brian Bockelman <bbockelman@morgridge.org> - 1.1.0-1
- Add support for Write and VectorRead operations
- Add support for checksum query and caching
- Add support for COPY verb to enable third-party-copy
- Fix the returned stat flags
- Fix uninitialized read when using the broker

* Wed Feb 5 2025 Brian Bockelman <bbockelman@morgridge.org> - 1.0.5-1
- Fix build failures with some compilers.

* Tue Feb 4 2025 Brian Bockelman <bbockelman@morgridge.org> - 1.0.4-1
- Fix potential segfault under load due to an incorrect lock being held.

* Fri Jan 3 2025 Brian Bockelman <bbockelman@morgridge.org> - 1.0.1-1
- Fix build issues on RHEL8

* Fri Jan 3 2025 Brian Bockelman <bbockelman@morgridge.org> - 1.0.1-1
- Fix build issues on RHEL9

* Thu Jan 2 2025 Brian Bockelman <bbockelman@morgridge.org> - 1.0.0-1
- Switch to using PROPFIND for stat, preventing opening a directory as a file
- Implement directory listings at the cache
- Cache the results of the director response, skipping director lookup when not needed
- Forward pelican.timeout header to the remote origin
- Fix a bug that invoked a callback twice, potentially segfaulting the process
- Add unit tests to the project

* Tue Sep 17 2024 Justin Hiemstra <jhiemstra@wisc.edu> - 0.9.4-1
- Provide error codes on  metadata lookup failure
- Allow the plugin to use X.509 authentication

* Thu Feb 8 2024 Mátyás Selmeci <matyas@cs.wisc.edu> - 0.9.3-2
- Add /etc/xrootd/client.plugins.d/pelican-plugin-http.conf

* Wed Feb 7 2024 Brian Bockelman <bbockelman@morgridge.org> - 0.9.3-1
- Add support for requesting reversed connections from the Pelican connection broker.
- Plugin no longer drops query parameters when `pelican://` is used; fixes
  issues with missing authorization from URL.

* Wed Jan 24 2024 Brian Bockelman <bbockelman@morgridge.org> - 0.9.2-1
- Add support for the `pelican://` protocol, allowing XCache to consume
  the federation metadata directly.

* Fri Jan 19 2024 Mátyás Selmeci <matyas@cs.wisc.edu> - 0.9.1-2
- Fix packaging to build on RHEL8 and RHEL9 as well

* Wed Dec 20 2023 Brian Bockelman <brian.bockleman@cern.ch> - 0.9.1-1
- Fix some undefined behavior on RHEL7 that could lead to a deadlock

* Sun Dec 10 2023 Brian Bockelman <brian.bockelman@cern.ch> - 0.9.0-1
- Initial packaging of the Pelican client

