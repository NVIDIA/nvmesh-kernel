#!/bin/bash
kind="$1"
sources_path="$2"
spec_path="$3"
spec_filename=`basename $spec_path`
installers_dir="$4"
target_dir="$5"
toma_udp="${6:-UNSET}" # if target rpm
rpm_build_type="$RPM_BUILD_TYPE" #exported: kmod/kmod_only/regular (default)
is_development="$IS_DEVELOPMENT" #exported via build.sh using build_sh_conf: IS_DEVELOPMENT=yes (default=no)
rpm_build_dir=`readlink -f ~/rpmbuild`
OLD_NVMESH_PREFIX="NVMesh-"
NVMESH_PREFIX="nvmesh-"
rpm_name=${NVMESH_PREFIX}${kind}
old_rpm_name=${OLD_NVMESH_PREFIX}${kind}
rpm_source_path="$rpm_build_dir/SOURCES/$rpm_name"
ARCH=`uname -m`
TOOLS_DIR=`realpath "./\`dirname "${BASH_SOURCE[0]}"\`/../tools/"`

source .config

trap 'rm -rf $sources_path $rpm_build_dir/SPECS/$spec_filename $rpm_source_path $rpm_build_dir/BUILD/$rpm_name* $rpm_build_dir/BUILDROOT/* $rpm_build_dir/RPMS/${ARCH}/$rpm_name*.${ARCH}.rpm > /dev/null 2>&1' EXIT

# If not already defined, get branch and commit_id from git
if [ -z "$BRANCH_NAME" ] ; then
        BRANCH_NAME=$(git symbolic-ref --short --quiet HEAD) || BRANCH_NAME=$(git rev-parse --abbrev-ref HEAD)
	if [ -z "$BRANCH_NAME" ]; then
		BRANCH_NAME=unknown
	fi
fi

if [ -z "$COMMIT_ID" ]; then
	COMMIT_ID=$(git log -n1 --format=%h)
	if [ -z "$COMMIT_ID" ]; then
		COMMIT_ID=unknown
	fi
fi

if [ -z "$VERSION" ] || [ -z "$RELEASE" ]; then
        GIT_DESCRIBE=$(git describe | cut -c 2-)

	IFS='-' read -ra git_describe <<< "$GIT_DESCRIBE"
	VERSION=${git_describe[0]}
	RELEASE=${git_describe[1]}
fi

if [ -z "$OFED_VER_STRING" ]; then
	OFED_VER_STRING=`ofed_info -s 2< /dev/null`
	if [ "x$OFED_VER_STRING" == "x" ]; then
		OFED_VER_STRING="none"
		OFED_REQUIRES=""
	else
		OFED_VER_STRING=${OFED_VER_STRING/%[\ :]*/}
		OFED_REQUIRES="ofed-scripts"
	fi
else
        OFED_REQUIRES="ofed-scripts"
fi

if [ -z "$DISTRIBUTION_INFO" ]; then
	DISTRIBUTION_INFO=`cat /etc/*release`
fi

if [ -z "$BLOCK_SIZE" ]; then
	BLOCK_SIZE="4KB"
fi

if [ -z "$KERN_VER" ]; then
	KERN_VER=`uname -r`
fi

KERN_VER_NO_ARCH=${KERN_VER%\.x86_64}

if [ -z "$VERSION" ]; then
	echo "Version was not specified! using default"
	VERSION=1.0.0
fi

if [ -z "$RELEASE" ]; then
	echo "Release was not specified! using default"
	RELEASE="1"
fi

function version8orAbove() {
	floorDistroVersion=`echo ${1} | cut -d"." -f1`

	if [ ${floorDistroVersion} -ge 8 ]; then return ; else false; fi
}

#python2-numpy - removing numpy as the dependency - too much headache
#managementCM.py performance may suffer, CPU usage will be high
#ubuntu & sles - python-numpy
declare -A translate_branch_to_version
translate_branch_to_version[master]='>= 2.0.3'

if [ -z "$UTILS_BRANCH" ] ; then
	UTILS_BRANCH='master'
fi

xz_version_cond=""
xz_version_cond_ubuntu=""

if [ "$is_development" != "yes" ]; then
	xz_version_cond=">= 5.2.4"
	xz_version_cond_ubuntu="($xz_version_cond)"
fi

utils_version=${translate_branch_to_version[$UTILS_BRANCH]}
librkfaka_minimal_version="1.6.1"

nvmeshBasicRequires="ethtool, util-linux, smartmontools, systemd"

if [ ${kind} == "exlog" ]; then
	RPM_REQUIRES="libibverbs, librdmacm"
	DEB_REQUIRES="libibverbs1, librdmacm1"
	SLES_REQUIRES="ofed-starget"
elif [ ${kind} == "base" ]; then # core
	RPM_REQUIRES="$nvmeshBasicRequires, nvmesh-utils $utils_version"
    DEB_REQUIRES="$nvmeshBasicRequires, nvmesh-utils ($utils_version)"
    SLES_REQUIRES="$nvmeshBasicRequires, nvmesh-utils $utils_version"
elif [ ${kind} == "client" ]; then # core
	RPM_REQUIRES="$nvmeshBasicRequires, kmod, xz $xz_version_cond, nvmesh-base >= $VERSION"
	DEB_REQUIRES="$nvmeshBasicRequires, kmod, xz-utils $xz_version_cond_ubuntu, nvmesh-base (>= $VERSION)"
	SLES_REQUIRES="$nvmeshBasicRequires, kmod, xz $xz_version_cond, nvmesh-base >= $VERSION"
elif [ ${kind} == "target" ]; then # core
	RPM_REQUIRES="$nvmeshBasicRequires, kmod, xz $xz_version_cond, pciutils, nvmesh-client >= $VERSION, librdkafka >= $librkfaka_minimal_version"
	DEB_REQUIRES="$nvmeshBasicRequires, kmod, xz-utils $xz_version_cond_ubuntu, pciutils, nvmesh-client (>= $VERSION), librdkafka1 (>= $librkfaka_minimal_version)"
	SLES_REQUIRES="$nvmeshBasicRequires, kmod, xz $xz_version_cond, pciutils, nvmesh-client >= $VERSION, librdkafka"
fi

cp $installers_dir/rpmmacros $HOME/.rpmmacros

if test -e /etc/redhat-release; then
	echo "Building RPM on Redhat-based distro. Running rpmdev-setuptree"
	if ! rpm -qa | grep -q rpmdevtools; then
		echo Required package rpmdevtools is not installed.
		echo Installing rpmdevtools once...
		sudo yum -y install rpmdevtools
	fi

	rpmdev-setuptree
elif test -e /etc/SuSE-release; then
	# SuSE doesn't have rpmdevtools so we have to create the dirs manually
	echo "Building RPM on SuSE distro. Manually setting up $rpm_build_dir"
	mkdir -p $rpm_build_dir/SPECS
	mkdir -p $rpm_build_dir/BUILD
	mkdir -p $rpm_build_dir/BUILDROOT
	mkdir -p $rpm_build_dir/SOURCES
	mkdir -p $rpm_build_dir/RPMS/$ARCH
	mkdir -p $rpm_build_dir/SRPMS

	cp $installers_dir/suse_rpmmacros $HOME/.rpmmacros
else
	echo "Unsupported distro - create_rpm.sh will probably not work"
	mkdir -p $rpm_build_dir/SPECS
	mkdir -p $rpm_build_dir/BUILD
	mkdir -p $rpm_build_dir/BUILDROOT
	mkdir -p $rpm_build_dir/SOURCES
	mkdir -p $rpm_build_dir/RPMS/$ARCH
	mkdir -p $rpm_build_dir/SRPMS
fi

# clean rpmbuild directories from leftovers of previous run
rm -rf $rpm_build_dir/BUILD/*
rm -rf $rpm_build_dir/BUILDROOT/*
rm -rf $rpm_build_dir/SOURCES/*

#check if rhel 8 and set the suitable dependencies
is_rhel=false
if [ "$DISTRO" == "RHEL" ]; then
	DISTRO_VER=`echo $DISTRO_VER | grep -Eo '[0-9]+\.[0-9]+'`
	is_rhel=true
elif [ -z "$DISTRO" ] && test -e /etc/redhat-release; then
	DISTRO_VER=`cat /etc/redhat-release | grep -Eo '[0-9]+\.[0-9]+'  | head -n1`
	is_rhel=true
fi

distro_tag=`$TOOLS_DIR/distro_tag.sh`

if [ -z "$distro_tag" ]; then
	echo "ERROR: could not find the distribution name and version to append to the RPM name"
fi

if [ -z "$PACKAGE_BUILD_NUMBER" ]; then
	PACKAGE_BUILD_NUMBER="buildnumber" #default place holder for non-official build
fi

RELEASE_WITHOUT_BUILD_NUM="$RELEASE$distro_tag"
RELEASE="$RELEASE_WITHOUT_BUILD_NUM.$PACKAGE_BUILD_NUMBER"

if [[ "$DISTRIBUTION_INFO" =~ "sles" ]]; then
	echo "sles"
	RPM_REQUIRES=$SLES_REQUIRES
fi

rpms_to_build="$kind"
if [ ! -z "$rpm_build_type" ]; then # KMOD IS NOT SUPPORTED ANYMORE - NEED TO ADJUST THE CODE IN CASE IT IS NEEDED AGAIN
	if [ "$rpm_build_type" = "kmod" ]; then
		rpms_to_build="core kmod-core"
		create_kmod=true
	elif [ "$rpm_build_type" = "kmod_only" ]; then
		rpms_to_build="kmod-core"
		create_kmod=true
	else
		create_kmod=false
	fi
else
	create_kmod=false
fi

for rpm_kind in $rpms_to_build; do
	if $create_kmod && [ "$rpm_kind" == "kmod-core" ]; then
		rpm_name="kmod-${NVMESH_PREFIX}core-${KERN_VER_NO_ARCH}"
		old_rpm_name="kmod-${OLD_NVMESH_PREFIX}core-${KERN_VER_NO_ARCH}"
		rpm_source_path="$rpm_build_dir/SOURCES/$rpm_name"
	fi

	# clear old source files from previous rpm creation
	rm -rf $rpm_source_path

	mkdir -p $rpm_source_path
	if $create_kmod; then
		if [ "$rpm_kind" == "kmod-core" ]; then
			spec_path=`dirname $3`
			spec_path=$spec_path/kmod-nvmesh-core.spec
			spec_filename=`basename $spec_path`

			echo Copying kmod sources
			cd $sources_path/client_*$KERN_VER
			if [ $? -ne 0 ]; then
				echo "ERROR: could not find client compilation output directory when creating kmod RPM"
				exit 1
			fi
			rsync -a . $rpm_source_path
			if [ $? -ne 0 ]; then
				echo "ERROR: missing client relevant files when creating kmod RPM"
				exit 1
			fi

			cd - > /dev/null 2>&1
			cd $sources_path/target_*$KERN_VER
			if [ $? -ne 0 ]; then
                                echo "ERROR: could not find target compilation output directory when creating kmod RPM"
                                exit 1
                        fi
			rsync -a --exclude='toma' . $rpm_source_path
			if [ $? -ne 0 ]; then
                                echo "ERROR: missing target relevant files when creating kmod RPM"
                                exit 1
                        fi

			cd - > /dev/null 2>&1
			cd $sources_path/common_*$KERN_VER
			if [ $? -ne 0 ]; then
                                echo "ERROR: could not find common compilation output directory when creating kmod RPM"
                                exit 1
                        fi
                        rsync -a --exclude='tools' --exclude='perfTest' . $rpm_source_path
                        if [ $? -ne 0 ]; then
                                echo "ERROR: missing common relevant files when creating kmod RPM"
                                exit 1
                        fi

                        cd - > /dev/null 2>&1
		else
			spec_path=`dirname $3`
			spec_path=$spec_path/nvmesh-core-env.spec
			spec_filename=`basename $spec_path`
			echo Copying sources
			rsync -a --exclude='target_*$KERN_VER*' --exclude='client_*$KERN_VER*' --exclude='common_*$KERN_VER*' --exclude='nvmesh_update' --exclude='scripts/target/nvme-cli/tests' $sources_path/* $rpm_source_path
			rsync -a --exclude='target_*/toma/bin/*/nvmeibt_toma.with_symbols' $sources_path/target_*/toma $rpm_source_path
			rsync -a $sources_path/common_*/tools $rpm_source_path
			scan_locks_path=`ls $sources_path/common_*/perfTest/io_stress/scan_locks/scan_locks_ec 2> /dev/null | head -1`
			if [ ! -z "$scan_locks_path" ]; then
				rsync -a $scan_locks_path $rpm_source_path/tools/
			fi
		fi
	else
		echo Copying sources
		rsync -a --exclude='target_*/toma/bin/*/nvmeibt_toma.with_symbols' --exclude='common_*/perfTest' --exclude='scripts/target/nvme-cli/tests' --exclude='management_cm/scaleSimulators' $sources_path/* $rpm_source_path
		scan_locks_path=`ls $sources_path/common_*/perfTest/io_stress/scan_locks/scan_locks_ec 2> /dev/null | head -1`
		if [ ! -z "$scan_locks_path" ]; then
			rsync -a $scan_locks_path $rpm_source_path/tools/
		fi
	fi

	if [ "$rpm_kind" == "client" ] || [ "$rpm_kind" == "target" ]; then
		echo Copying post-install and uninstall scripts.
		cp $installers_dir/post_install $rpm_source_path
		cp $installers_dir/post_install_ib_core_mod $rpm_source_path
		cp $installers_dir/install.py $rpm_source_path
		cp $installers_dir/uninstall-$rpm_kind $rpm_source_path

		#making sure all the upgrade scripts have executable permissions before packaging them
		echo "Chmoding upgrade scripts..."
		chmod +x $rpm_source_path/upgrade_scripts/*/*

		#filtering out upgrade scripts not in the correct version format
		find "$rpm_source_path/upgrade_scripts/" -maxdepth 2 ! -name '[0-9a-zA-Z]*' -type f -exec rm -f {} +

		echo "Changing ko.xz suffix to ko.xz.bcp to avoid kernel xz recognition"
		compressed_kos=`find $rpm_source_path -name '*.ko.xz' -type f 2>/dev/null`
		if [ ! -z "$compressed_kos" ]; then
			# renaming the ko.xz to ko.xz.bcp so the kernel would not be able to load it (allow only ko load while making the decompress only one time)
			for ko_xz in $compressed_kos; do
				mv "$ko_xz" "$ko_xz.bcp"
			done
		fi
	fi

	echo Copying specfile
	cp $spec_path $rpm_build_dir/SPECS/
	echo "Building $rpm_kind RPM..."

	rpmbuild -v $rpmbuild_extra_flags --define "branch $BRANCH_NAME" --define "commit_id $COMMIT_ID" --define "block_size $BLOCK_SIZE" --define "kern_ver $KERN_VER" --define "kern_ver_no_arch $KERN_VER_NO_ARCH" --define "ofed_ver $OFED_VER_STRING" --define "toma_udp $toma_udp" --define "requires_pkgs $RPM_REQUIRES" -ba --buildroot=$rpm_build_dir/BUILDROOT $rpm_build_dir/SPECS/$spec_filename --define "version $VERSION" --define "release $RELEASE" --define "__strip $(dirname $spec_path)/strip_wrapper.sh" 2>&1

	rpm_creation_retval=$?
	if [ $rpm_creation_retval -ne 0 ]; then
		echo "rpmbuild failed with rc=$rpm_creation_retval"
		exit 1
	fi

	echo Removing old RPM if exists
	rm -f $target_dir/$rpm_name-$VERSION-$RELEASE_WITHOUT_BUILD_NUM*.$ARCH.rpm $target_dir/$old_rpm_name-$VERSION-$RELEASE_WITHOUT_BUILD_NUM*.$ARCH.rpm

	echo Bringing the RPM...
	cp $rpm_build_dir/RPMS/${ARCH}/$rpm_name* $target_dir

	if [ "$rpm_creation_retval" -eq "0" ] && [ "$SIGN_RPM" == true ]; then
		new_rpm="$target_dir/$rpm_name*$VERSION-$RELEASE*.rpm"
		echo "Signing RPM..."
		rpm --addsign $new_rpm
		if [ "$?" -ne "0" ]; then
			echo "Failed to sign RPM!"
		fi
	fi

	if [[ "$DISTRIBUTION_INFO" =~ "Ubuntu" ]] || [ "$DISTRO" == "Ubuntu" ]; then
		echo Removing old DEB if exists
		rm -f $target_dir/$rpm_name*.deb

		ubuntu_dir="ubuntu_deb_build"
		echo "Building Ubuntu deb package..."
		mkdir -p $ubuntu_dir
		cd $ubuntu_dir

		if [ $ARCH == aarch64 ]; then
			ATARGET=--target=arm64
		else
			ATARGET=
		fi
		fakeroot alien --generate -k --script $target_dir/$rpm_name*.rpm $ATARGET

		packDir=$rpm_name-$VERSION

		if [ -e $packDir/debian ]; then
			echo "Configuring deb dependencies..."
			sed -i -E "s/^[ ]*Depends:\s.*$/Depends: $DEB_REQUIRES/g" $packDir/debian/control

			echo "Setting gzip compression..."
			sed -i -E "s/dh_builddeb$/dh_builddeb -- -Zgzip/g" $packDir/debian/rules

			cd $packDir
			dpkg-buildpackage -uc -us -d
			cd ..
			fakeroot rm -rf $packDir
			cp *.deb $target_dir
		fi
		cd ..
		rm -rf $ubuntu_dir
		cd $target_dir
		rm -f $target_dir/$rpm_name*.rpm
	fi

	echo Cleaning up...
	rm -rf $rpm_build_dir/SPECS/$spec_filename $rpm_source_path $rpm_build_dir/BUILD/$rpm_name* $rpm_build_dir/BUILDROOT/* $rpm_build_dir/RPMS/${ARCH}/$rpm_name*.${ARCH}.rpm
done
