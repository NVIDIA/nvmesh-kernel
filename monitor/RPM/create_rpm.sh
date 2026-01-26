#!/bin/bash

source ../transfer_lists
MONITOR_ROOT=$(dirname $(readlink -e $0))/..
NVMESH_ROOT=$MONITOR_ROOT/..
INFRA_ROOT=$NVMESH_ROOT/../infrastructure

print_help() {
cat << EOF
usage:

-h      --help              prints this help

-b      --branch            branch name

-c      --commit-id         commit id

-g      --change-id         change id

-d      --git-describe      git describe string

-S	--sign-rpm	    sign the rpm

-e      --no-executable     do not make executables

--dist-tag		    distribution tag to include in the RPM name

--build-number              package build number

--ubuntu		    create deb package for ubuntu

--distroless		    build distroless container with the deb package
EOF
}

buildRPMSetupTree() {
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
    else
        echo "Unsupported distro - create_rpm.sh will probably not work"
        mkdir -p $rpm_build_dir/SPECS
        mkdir -p $rpm_build_dir/BUILD
        mkdir -p $rpm_build_dir/BUILDROOT
        mkdir -p $rpm_build_dir/SOURCES
        mkdir -p $rpm_build_dir/RPMS/$ARCH
        mkdir -p $rpm_build_dir/SRPMS
    fi
}

pkgDeb() {
    if $isUbuntu || [[ "$DISTRIBUTION_INFO" =~ "Ubuntu" ]]; then
        which alien
        if [ $? -ne 0 ];then
            echo "alien is not installed, deb package will not be built"
            return
        fi

        ubuntu_dir="ubuntu_deb_build"
        echo "Building Ubuntu deb package..."
        mkdir $ubuntu_dir
        cd $ubuntu_dir

        if [ $ARCH == aarch64 ]; then
            ATARGET=--target=arm64
        else
            ATARGET=
        fi

        alien --generate -k --scripts ~/"rpmbuild/RPMS/$ARCH/$packageName"* $ATARGET

        packDir=$packageName-$rpm_version

        if [ -e $packDir/debian ]; then
            echo "Setting gzip compression..."
            sed -i -E "s/dh_builddeb$/dh_builddeb -- -Zgzip/g" $packDir/debian/rules

            cd $packDir
            dpkg-buildpackage -uc -us
            cd ..
            rm -rf $packDir
            cp *.deb ../
        fi

        cd ..
        rm -rf $ubuntu_dir
    fi
}

isUbuntu=false
buildDistroless=false
buildNum="buildnumber" #default place holder for non-official build
tools_to_make="nvmesh_exporter"

while [[ $# -gt 0 ]]
do
key="$1"

case $key in
    -h|--help)
    print_help
    exit 0
    ;;
    -b|--branch)
    branchName="$2"
    shift
    ;;
    -c|--commit-id)
    commitID="$2"
    shift
    ;;
    -g|--change-id)
    changeID="$2"
    shift
    ;;
    -d|--git-describe)
    describe="$2"
    shift
    ;;
    -S|--sign-rpm)
    signRPM=true
    ;;
    --build-number)
    buildNum="$2"
    shift
    ;;
    --dist-tag)
    distTag="$2"
    shift
    ;;
    --ubuntu)
    isUbuntu=true
    ;;
    --distroless)
    buildDistroless=true
    isUbuntu=true  # distroless requires deb package for now
    ;;
    --infra-branch)
    infraBranchName="$2"
    shift
    ;;
    --infra-commit-id)
    infraCommitID="$2"
    shift
    ;;
    *)
    # unknown option
    echo "Unknown option $key"
    print_help
    exit 1
    ;;
esac
shift # past argument or value
done

if [ -z "$DISTRIBUTION_INFO" ]; then
    DISTRIBUTION_INFO=`cat /etc/*release`
fi

if [ -z $commitID ] ; then
    commitID=$(git log -n1 --format=%h)
fi

if [ -z $changeID ] ; then
    changeID=$(git log -n1 --format=%b | awk '/^Change-Id: / {print $2}')
fi

if [ -z $branchName ] ; then
    branchName=`git rev-parse --abbrev-ref HEAD 2> /dev/null`
fi

if [ -z $infraCommitID ] ; then
    infraCommitID=$(GIT_DIR=$INFRA_ROOT/.git git log -n1 --format=%h)
fi

if [ -z $infraBranchName ] ; then
    infraBranchName=$(GIT_DIR=$INFRA_ROOT/.git git rev-parse --abbrev-ref HEAD 2> /dev/null)
fi

if [ -z $distTag ] ; then
    if [ -e /etc/redhat-release ]; then
        redhat_rel_content=`cat /etc/redhat-release`
        if [[ "$redhat_rel_content" =~ ([0-9]+).([0-9]+) ]]; then
            minor_ver=${BASH_REMATCH[2]}
            rpm_dist_tag=`rpm --eval='%{?dist}'`
            if [ -z "$minor_ver" ] || [ -z "$rpm_dist_tag" ]; then
                echo "ERROR: cannot find the distribution version"
                exit 1
            fi

            # check where to place the minor version
            IFS='.' read -r -a rpm_dist_tag_arr <<< "$rpm_dist_tag"

            if [ ${#rpm_dist_tag_arr[@]} -eq 3 ]; then
                distTag=".${rpm_dist_tag_arr[1]}"_"$minor_ver"
            else
                distTag="$rpm_dist_tag"_"$minor_ver"
            fi
        fi
    else
        #not rhel related os - ubuntu or other
        os_release_content=`cat /etc/os-release`
        if [[ "$os_release_content" =~ VERSION_ID=\"([^\"]*) ]]; then
            os_version_id=${BASH_REMATCH[1]}
            short_version_id=`echo $os_version_id | tr -d .`

            if [[ "$os_release_content" =~ NAME=\"([^\"]*) ]]; then
                os_name=${BASH_REMATCH[1]}
                # convert to lower case
                lower_os_name=`echo "$os_name" | awk '{print tolower($0)}'`
                # remove spaces
                lower_os_name=${lower_os_name//[[:blank:]]/}
                distTag=".$lower_os_name$short_version_id"
            fi
        fi
    fi

    if [ -z "$distTag" ]; then
        echo "ERROR: could not find the distribution name and version to append to the RPM name"
    fi
fi

if [ -z $describe ] ; then
    describe=$(git describe | cut -c 2-)
elif [[ $describe == v* ]]; then
    describe=$(echo $describe | cut -c 2-)
fi

IFS='-' read -ra gitDescribe <<< "$describe"

rpm_version="${gitDescribe[0]}"
rpm_release_num="${gitDescribe[1]}"
rpm_release="${rpm_release_num}$distTag.$buildNum"

echo "VERSION: $rpm_version RELEASE: $rpm_release"

rpm_build_dir=`readlink -f ~/rpmbuild`
ARCH=`uname -m`

buildRPMSetupTree

command -v rpmbuild >/dev/null 2>&1 || { echo rpm creator require rpmbuild but it is not installed.  Aborting. >&2; exit 1; }

#cp rpmmacros $HOME/.rpmmacros

packageName="nvmesh-monitor"
cp nvmesh-monitor.spec ~/rpmbuild/SPECS/

rm ~/rpmbuild/RPMS/$ARCH/$packageName*$ARCH.rpm

cd ../
# bulid monitor tools
rm -rf dist/
echo "nvmesh_commit_id:$commitID branch:$branchName infra_commit_id:$infraCommitID infra_branch:$infraBranchName" > git_spec.info
TOOLS_CONF=$MONITOR_ROOT/tools.yaml $NVMESH_ROOT/py_to_exec.sh

if [ $? -ne 0 ]; then
    echo "ERROR: failed to make monitor tools"
    exit 1
fi

rsync -aRPq --exclude=*.pyc --exclude=*~ $monitor_transfer_list ~/"rpmbuild/SOURCES/$packageName"
cp -R ../public ~/"rpmbuild/SOURCES/$packageName"
cd RPM

rpmbuild -ba ~/rpmbuild/SPECS/nvmesh-monitor.spec --define "commit_id $commitID" --define "change_id $changeID" --define "branch $branchName" --define "version $rpm_version" --define "release $rpm_release" --define "infra_branch $infraBranchName" --define "infra_commit_id $infraCommitID" --define "packaged_tools $tools_to_make"
rpm_creation_retval=$?

cp ~/"rpmbuild/RPMS/$ARCH/$packageName"* .

rm -rf ~/rpmbuild/SPECS/nvmesh-monitor.spec ~/"rpmbuild/SOURCES/$packageName" ~/"rpmbuild/BUILD/"* ~/"rpmbuild/BUILDROOT/"*

if [ "$rpm_creation_retval" -eq "0" ] && [ "$signRPM" == true ]; then
    new_rpm="$packageName*$rpm_version-$rpm_release*.rpm"
    echo "Signing RPM..."
    rpm --addsign $new_rpm
    
    if [ "$?" -ne "0" ]; then
        echo "Failed to sign RPM!"
    fi
fi

pkgDeb

if [ "$buildDistroless" = true ]; then
    deb_file=$(ls -1 ${packageName}*.deb 2>/dev/null | head -1)
    
    if [ -z "$deb_file" ]; then
        echo "ERROR: No deb file found for distroless container build"
        exit 1
    fi
    
    SCRIPT_DIR=$(dirname $(readlink -e $0))
    "$SCRIPT_DIR/build_distroless.sh" "$deb_file"
fi
