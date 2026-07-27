#!/usr/bin/python3
"""
Utils for udev.
"""
import sys
import os
import re
import subprocess

BUNDLE_MODE = hasattr(sys, '_MEIPASS')
if not BUNDLE_MODE:
    sys.path.append('/opt/nvmesh/client-repo')
from management_cm import nvmesh_script_utils
    
symlink_by_id = 'disk/by-id/'
symlink_by_id_scsi = 'disk/by-id/scsi-'
symlink_by_scsid = 'disk/by-scsid/'
symlink_name = '{}'
symlink_uuid = '{}-vol-{}-{}'
dev_name_format = 'nvmesh-{}-{}'


class EmptyLogger:
    """
    For simple usage of nvmesh_script_utils.
    """

    def info(self, *args, **kwargs):
        pass

    def error(self, *args, **kwargs):
        pass

    def debug(self, *args, **kwargs):
        pass


def split_dev_path(dev_path):
    try:
        dev_root, volume = dev_path.split('/')  # example: nvmesh/vol0
        return dev_root, volume
    except ValueError:
        return None, None  # device might not match nvmesh device


def get_script_utils_by_dev_root(dev_root):
    logger = EmptyLogger()
    script_utils = nvmesh_script_utils.MultiClientUtils(
        logger, multi_client_instance=None, dev_root=dev_root)

    if not script_utils.instance_name:
        return None  # for non-nvmesh devices

    return script_utils


def gen_name(dev_root, volume):
    script_utils = get_script_utils_by_dev_root(dev_root)
    if not script_utils:
        return 0

    name = dev_name_format.format(script_utils.instance_name, volume)
    print(name)
    return 0


def gen_devlink_name(volume, symlink_type):
    dev_symlink = symlink_type + symlink_name.format(volume)
    print(dev_symlink)
    return 0


def gen_devlink_uuid(symlink_type, symlink_prefix, cluster_uuid, volume_uuid):
    dev_symlink = symlink_type + symlink_uuid.format(symlink_prefix, cluster_uuid, volume_uuid)
    print(dev_symlink)
    return 0


def set_debug_di(dev_root, volume):
    script_utils = get_script_utils_by_dev_root(dev_root)
    if not script_utils:
        return 0

    nvmesh_conf = nvmesh_script_utils.nvmeshConfigFileParams
    if 'DEFAULT_DEBUG_DI' in nvmesh_conf:
        nvmesh_conf_debug_di = nvmesh_conf['DEFAULT_DEBUG_DI'].lower()
        if nvmesh_conf_debug_di == 'no':
            value = 0
        elif nvmesh_conf_debug_di == 'yes':
            value = 1
        else:
            return 0

        cmd = '#%s|di_debug_mode=%d' % (volume, value)
        try:
            with script_utils.open_proc_file() as f:
                try:
                    os.write(f, cmd)
                except OSError as error:
                    print(error.message)
                    return 1
        except:
            print('Could not open client cli')
            return 1


def gen_scsi_link(volume, symlink_type, symlink_prefix):
    try:
        cmd = "sg_inq /dev/{}/{}".format(symlink_prefix, volume)
        with open(os.devnull, 'w') as devnull:
            sg_inq_out = subprocess.check_output(cmd, shell=True, stderr=devnull)
        vendor = re.findall(r'\bVendor identification:\s([^\n\r]*)', sg_inq_out)[0].replace(" ", "_")
        product = re.findall(r'\bProduct identification:\s([^\n\r]*)', sg_inq_out)[0].replace(" ", "_")
        serial = re.findall(r'\bUnit serial number:\s([^\n\r]*)', sg_inq_out)[0].replace(" ", "_")
        scsi_symlink = symlink_type + 'S' + vendor + product + "_" + serial
        print(scsi_symlink)
        return 0
    except:
        return 1


cmd_map = {'gen-name': gen_name,
           'by-id': gen_devlink_uuid,
           'debug-di': set_debug_di,
           'by-id-scsi': [gen_devlink_name, gen_devlink_uuid, gen_scsi_link],
           'by-scsid': [gen_devlink_name, gen_devlink_uuid, gen_scsi_link]
           }


def main(argv):
    if len(argv) < 2:
        print('Missing udev util')
        return 1

    udev_util = argv[1]
    util_args = argv[2:]

    if udev_util in cmd_map:
        if len(util_args) != 1:
            print('{} receives a single argument'.format(udev_util))
            return 1

        dev_path = util_args[0]
        dev_root, volume = split_dev_path(dev_path)
        if not dev_root or not volume:
            return 0
        script_utils = get_script_utils_by_dev_root(dev_root)
        if not script_utils:
            return 0

        cluster_uuid = script_utils.get_cluster_uuid()
        volume_uuid = script_utils.name_to_uuid(volume)
        symlink_prefix = dev_root if dev_root == 'nvmesh' else script_utils.instance_name

        if udev_util in ['gen-name', 'debug-di']:
            cmd_map[udev_util](dev_root, volume)
        elif udev_util == 'by-id':
            cmd_map[udev_util](symlink_by_id, symlink_prefix, cluster_uuid, volume_uuid)
        elif udev_util == 'by-id-scsi':
            cmd_map[udev_util][0](volume, symlink_by_id_scsi)
            cmd_map[udev_util][1](symlink_by_id_scsi, symlink_prefix, cluster_uuid, volume_uuid)
            cmd_map[udev_util][2](volume, symlink_by_id_scsi, dev_root)
        elif udev_util == 'by-scsid':
            cmd_map[udev_util][0](volume, symlink_by_scsid)
            cmd_map[udev_util][1](symlink_by_scsid, symlink_prefix, cluster_uuid, volume_uuid)
            cmd_map[udev_util][2](volume, symlink_by_scsid, dev_root)
        else:
            return 1
    else:
        print('Invalid util: {}.  Allowed utils: {}'.format(udev_util, ", ".join(cmd_map.keys())))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
