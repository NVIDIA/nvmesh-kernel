%define _build_id_links none
%global __python /usr/bin/python3
%global __python3 /usr/bin/env python3

Name:                nvmesh-monitor
Version:             %{version}
Release:             %{release}
Summary:             "nvmesh-monitor" by NVIDIA

License:             Apache-2.0
URL:                 http://www.nvidia.com
Source0:             %{name}

%description
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"NVIDIA nvmesh-monitor" includes NVMesh monitor set of tools.
    Branch: %{branch}
    Commit: %{commit_id}
    Infra Branch: %{infra_branch}
    Infra Commit: %{infra_commit_id}
    Packaged Tools: %{packaged_tools}

%prep
cp -rf %{_sourcedir}/%{name} %{_builddir}/

%build
# Nothing to build

%install
# Create base directories
mkdir -pv %{buildroot}/opt/nvmesh/monitor
mkdir -pv %{buildroot}/var/log/nvmesh/monitor
mkdir -pv %{buildroot}/lib/systemd/system
mkdir -pv %{buildroot}/etc/nvmesh/

# Move service file
mv %{_builddir}/%{name}/nvmesh_exporter/nvmeshexporter.service %{buildroot}/lib/systemd/system/

# Copy all files from source
cp -rf %{_builddir}/%{name}/* %{buildroot}/opt/nvmesh/monitor

# Version info
echo "version=\"%{version}-%{release}\"" > %{buildroot}/opt/nvmesh/monitor/version
echo "commit=\"%{commit_id}\"" >> %{buildroot}/opt/nvmesh/monitor/version
echo "branch=\"%{branch}\"" >> %{buildroot}/opt/nvmesh/monitor/version

# Runtime log file
touch %{buildroot}/var/log/nvmesh/monitor/nvmesh_metrics.out

# Prepare files.lst
echo "%dir /opt/nvmesh
%dir /var/log/nvmesh
%dir /var/log/nvmesh/monitor
%dir /etc/nvmesh
/lib/systemd/system/nvmeshexporter.service
/opt/nvmesh/monitor/*
%ghost /var/log/nvmesh/monitor/nvmesh_metrics.out" > files.lst

%post
# Symlink for Python exporter
ln -sf /opt/nvmesh/monitor/dist/pytools/nvmesh_exporter /opt/nvmesh/monitor/nvmesh_exporter
systemctl daemon-reload


%postun
systemctl daemon-reload

# Detect Ubuntu upgrade
isUpgrade=$1
if grep -i Ubuntu /etc/*release > /dev/null 2>&1; then
    if [ "$isUpgrade" == "configure" ] && [ -z "$2" ]; then
        isUpgrade=1
    elif [ "$isUpgrade" == "abort-upgrade" ] || [ "$isUpgrade" == "abort-remove" ]; then
        exit 0
    else
        isUpgrade=2
    fi
fi

%preun
# Not upgrade
if [ "$1" = "0" -o "$1" = "remove" ]; then
    isSysD=false
    if pidof systemd > /dev/null 2>&1; then
        isSysD=true
    fi

    progs=("nvmeshexporter")
    for prog in ${progs[@]}; do
        service $prog stop

        if $isSysD && systemctl is-failed $prog > /dev/null 2>&1; then
            systemctl reset-failed $prog
        fi
    done

    # Remove runtime directories safely
    rm -rf /opt/nvmesh/monitor
    rm -rf /var/log/nvmesh/monitor
fi

%files -f files.lst

# %config(noreplace) /etc/nvmesh/nvmesh-fluent-bit.conf

%changelog
* Wed Jul 6 2022 NVIDIA
- Installing NVIDIA nvmesh-monitor
