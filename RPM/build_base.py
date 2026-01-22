#!/usr/bin/python2

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import sys, subprocess, errno, getopt, json, os.path, os, time

global image_description_dic
image_description_dic = {}
global failed_images
failed_images = []

def clean_failed_images():
    fail_to_del = []
    for image_to_del in failed_images:
        print "Deleting %s VM and storage file" % image_to_del
        clean_command = "sudo virsh undefine {0}; sudo rm -f /mnt/ext4/vm-images-new/{0}.qcow2".format(image_to_del)
        del_proc = subprocess.Popen(clean_command, shell=True)
        proc_res = del_proc.wait()
        if not proc_res == 0:
            print "Unable to delete %s image" % image_to_del
            fail_to_del.append(image_to_del)
        else:
            print  "%s was deleted \n" % image_to_del
    if len(fail_to_del) != 0:
        print "Failed to delete those images:"
        for fail in fail_to_del:
            print "%s\n" % fail_to_del[fail]


def create_log_file(file_name):
    global LOG_FILE
    LOG_FILE = open(file_name + '.log', 'w+')


def dumpclean(obj):
    if type(obj) == dict:
        for k, v in obj.items():
            if hasattr(v, '__iter__'):
                print k
                dumpclean(v)
            else:
                print '%s : %s' % (k, v)
    elif type(obj) == list:
        for v in obj:
            if hasattr(v, '__iter__'):
                dumpclean(v)
            else:
                print v
    else:
        print obj


def build_base_images():
    existing_clones_array = os.popen('sudo virsh list --all --name').read().split('\n')

    with open(METRIC_FILE) as data_file:
        data = json.load(data_file)

    compile_processes_arr = []
    success_statuses = {}
    proc_counter = 0
    PROC_COUNTER_LIMIT = 10
    inbox_driver = False
    for distro in data:
        for image, combos in data[distro]["distributions_images"].iteritems():
            success_statuses[image] = []
            single_image_desc = {}
            single_image_desc['image'] = image
            single_image_desc['ofeds'] = ''
            single_image_desc['kernels'] = ''

            for ofed_tgz, kernels in combos.iteritems():
                inbox_driver = False
                if ofed_tgz == 'none':
                    inbox_driver = True
                single_image_desc['ofeds'] = single_image_desc['ofeds'] + ' ' + ofed_tgz

                for kernel in kernels:
                    if kernel not in single_image_desc['kernels']:
                        single_image_desc['kernels'] = single_image_desc['kernels'] + ' ' + kernel

                    log_file_name = "%s-%s-%s_base" % (image, ofed_tgz, kernel)
                    create_log_file(log_file_name)
                    if inbox_driver:
                        ofed_ver = 'inbox_driver'
                    else:
                        ofed_ver = ofed_tgz.split('-')
                        ofed_ver = 'o' + ofed_ver[1] + '-' + ofed_ver[2];
                    if distro == "EL":
                        short_kernel = kernel.replace(".x86_64", "")
                    else:
                        short_kernel = kernel
                    clone_name = image + '-' + ofed_ver + '-k' + short_kernel + '_base'
                    print "\n" + clone_name
                    got_clone = False
                    clone_exists = False

                    if clone_name not in existing_clones_array:
                        print "Trying to clone image: %s" % clone_name
                        proc = subprocess.Popen(
                            "cd /var/lib/libvirt/images/; sudo time virt-clone --name={1} --original={0} --file /mnt/ext4/vm-images-new/{1}.qcow2  2>&1".format(
                                image, clone_name), shell=True)
                        proc_res = proc.wait()

                        if not proc_res != 0:
                            got_clone = True
                    elif clone_name in existing_clones_array and not UPDATE_PKGS:
                        print "Clone already exists"
                        clone_exists = True
                    else:
                        clone_exists = True
                        got_clone = True

                    if got_clone:
                        print "Got clone!!!"
                        compile_proc = {}
                        print "==========PARAMETERS FOR build_clone.sh: -c %s -i %s -k %s -o %s/%s -u %s" % (
                        clone_name, image, kernel, OFED_DIR, ofed_tgz, PKGS_TO_UPDATE)
                        build_command = BUILD_SCRIPT_FILE + " -c %s -i %s -k %s -o %s%s -u %s" % (
                        clone_name, image, kernel, OFED_DIR, ofed_tgz, PKGS_TO_UPDATE)

                        compile_proc["process"] = subprocess.Popen(build_command, stdout=LOG_FILE, stderr=LOG_FILE,
                                                                   shell=True)
                        compile_proc["sys_name"] = clone_name
                        compile_proc["image"] = image
                        compile_proc["is_done"] = False
                        proc_counter = proc_counter + 1

                        compile_processes_arr.append(compile_proc)
                        print "New process started:" + compile_proc["sys_name"]
                        if proc_counter == PROC_COUNTER_LIMIT:
                            print "Passed processes limit of:" + str(PROC_COUNTER_LIMIT)
                            proc_finished = False
                            while not proc_finished:
                                for proc in compile_processes_arr:
                                    if not proc["is_done"]:
                                        res = proc["process"].poll()
                                        if res != None:
                                            print "This process has finished: " + proc["sys_name"]
                                            proc_finished = True
                                            proc["is_done"] = True
                                            proc["ret_code"] = res
                                            proc_counter = proc_counter - 1
                                            break
                                time.sleep(5)
                    else:
                        if not clone_exists:
                            success_statuses[image].append({compile_proc["sys_name"]: False})
                            failed_images.append(compile_proc["sys_name"])
                            print "clone not succeeded for: %s" % clone_name

    image_description_dic[image] = single_image_desc
    for cproc in compile_processes_arr:
        image = cproc["image"]
        if cproc["is_done"] == False:
            print "waiting for proc:" + cproc["sys_name"]
            cproc["ret_code"] = cproc["process"].wait()

        if cproc["ret_code"] != 0:
            success_statuses[image].append({cproc["sys_name"]: False})
            failed_images.append(cproc["sys_name"])
            print 'Error while trying to build %s' % cproc["sys_name"]
        else:
            success_statuses[image].append({cproc["sys_name"]: True})
            print 'Success! Build %s successfully' % cproc["sys_name"]

    return success_statuses


def main(argv):
    try:
        opts, args = getopt.getopt(argv, "hb:m:o:d:k:u:", ["help", "build-script-file=", "dist-ofed-kernel-metric-file=",
                                                         "ofed-tgz-dir=", "outputdir=", "kvm-images-dir=", "update-packages="])
    except getopt.GetoptError:
        print 'build_base.py -b <build-script-file> -m <dist-ofed-kernel-metric-file> -o <ofed-tgz-dir> -d <outputdir>' \
              ' -k <kvm-images-dir> -u <additional packages to existing VM>"'
        sys.exit(2)

    global UPDATE_PKGS
    UPDATE_PKGS = False
    global PKGS_TO_UPDATE
    PKGS_TO_UPDATE = "none"
    for opt, arg in opts:
        if opt == '-h':
            print 'build_base.py -b <build-script-file> -m <dist-ofed-kernel-metric-file> -o <ofed-tgz-dir>' \
                  '  -k <kvm-images-dir> -u <additional packages to existing VM>'
            sys.exit()
        elif opt in ("-b", "--build-script-file="):
            global BUILD_SCRIPT_FILE
            BUILD_SCRIPT_FILE = arg
        elif opt in ("-m", "--dist-ofed-kernel-metric-file"):
            global METRIC_FILE
            METRIC_FILE = arg
        elif opt in ("-o", "--ofed-tgz-dir"):
            global OFED_DIR
            OFED_DIR = arg
        elif opt in ("-k", "--kvm-images-dir"):
            global KVM_IMAGES_DIR
            KVM_IMAGES_DIR = arg
        elif opt in ("-u", "--update-packages"):
            PKGS_TO_UPDATE = arg
            UPDATE_PKGS = True
    success_statuses = build_base_images()
    print "Status per image:"
    dumpclean(success_statuses)
    clean_failed_images()

if __name__ == "__main__":
    main(sys.argv[1:])
