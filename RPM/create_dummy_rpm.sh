#!/bin/bash

if [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
	echo "USAGE:"
	echo "./create_dummy_rpm <version> <release>        (if not passed the version and release will be taken from 'git-describe')"
	echo
	echo "EXAMPLE: ./create_dummy_rpm 3.0.0 436"
	exit 0
fi

VERSION="$1"
RELEASE="$2"
OLD_EXCELERO_PREFIX="NVMesh-"
EXCELERO_PREFIX="nvmesh-"
ARCH=`uname -m`
script_path=`readlink -f $0`
script_dir=`dirname $script_path`
target_dir=$script_dir
rpm_build_dir=`readlink -f ~/rpmbuild`

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


RPM_REQUIRES="nvmesh-client >= $VERSION-$RELEASE, nvmesh-target >= $VERSION-$RELEASE"
DEB_REQUIRES="nvmesh-client (>= $VERSION-$RELEASE), nvmesh-target (>= $VERSION-$RELEASE)"


cp $script_dir/rpmmacros $HOME/.rpmmacros

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

    cp $script_dir/suse_rpmmacros $HOME/.rpmmacros
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

kind="core"
rpm_name=${EXCELERO_PREFIX}${kind}
rpm_source_path="$rpm_build_dir/SOURCES/$rpm_name"
spec_file="${OLD_EXCELERO_PREFIX}${kind}.spec"

mkdir $rpm_source_path

if [ ! -d $rpm_build_dir/SPECS ]; then
	mkdir $rpm_build_dir/SPECS
fi

echo Copying specfile
cp $spec_file $rpm_build_dir/SPECS/

echo Building RPM

if [[ "$DISTRIBUTION_INFO" =~ "sles" ]]; then
	echo "sles"
	RPM_REQUIRES=$SLES_REQUIRES
fi

rpmbuild --define "branch $BRANCH_NAME" --define "commit_id $COMMIT_ID" --define "requires_pkgs $RPM_REQUIRES" -ba --buildroot=$rpm_build_dir/BUILDROOT $rpm_build_dir/SPECS/$spec_file --define "version $VERSION" --define "release $RELEASE" 2>&1

rpm_creation_retval=$?

echo Removing old RPM if exists
rm -f $target_dir/$rpm_name-$VERSION-$RELEASE.$ARCH.rpm

echo Bringing the RPM...
cp $rpm_build_dir/RPMS/${ARCH}/${EXCELERO_PREFIX}${kind}* .

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
