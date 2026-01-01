#!/usr/bin/env python
import json, yaml, glob, time, socket, re, os, sys, jmespath, urllib3, subprocess, shutil, signal, threading
from copy import deepcopy
import logging.handlers
from urllib.parse import urlparse
from typing import List, Optional

from packaging.version import Version, InvalidVersion
from collections import defaultdict
from collections.abc import Iterable, Mapping
from abc import abstractmethod
from argparse import ArgumentParser
from concurrent.futures import ThreadPoolExecutor
from functools import reduce
from threading import Lock, RLock
from prometheus_client import start_http_server, Gauge
from prometheus_client.metrics import MetricWrapperBase
from prometheus_client.registry import REGISTRY

from typing import Any, Optional, Tuple, List, Dict, Union, Set, Type
from xlro.core import infra_conf
from xlro.core.entities import Manager, Host
from xlro.core.sdk.ConnectionManager import ConnectionManager, ConnectionManagerError
from xlro.core.util.general_utils import host_name, wait_for_it
from xlro.core.util.ssh import Connection
from xlro.core.util.dict_util import merge_dicts, del_path
import tracemalloc, psutil

# Until we configure security properly, ignore certificate warnings
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

GIT_SPEC = {}
EXPORTER_VERSION = 'v2.3.1'  # support labels list
Connection.LOCALHOST_CHECK = True

# TODO: move to some utils lib once it's completely in nvmesh repo
def get_path(orig_path: str, pyinstaller_location: str = "") -> str:
    if hasattr(sys, '_MEIPASS'):
        return os.path.join(sys._MEIPASS, pyinstaller_location)  # type: ignore[attr-defined]
    return orig_path


# The following consts are actually defaults and can be overridden by get_default() in nvmesh.conf or arguments
SERVER_PORT = 9300
INTERVAL = 10
DEL_THRESHOLD = 1
ADDITIONAL_LABELS = ''
METRICS_CONF_PATH = get_path(f'{os.path.dirname(__file__)}/nvmesh_exporter_conf.yaml', 'nvmesh_exporter_conf.yaml')
LOGGING_LEVEL = logging.INFO
LOGGING_FILE = '/var/log/nvmesh/monitor/monitor_metrics.out'
MAX_CONCURRENT_FOPEN = 10
MANAGEMENT_USER = 'monitor'
MANAGEMENT_CREDS_FILE = None
MANAGEMENT_USE_TLS = False
MANAGEMENT_TLS_CERT = '/etc/nvmesh/tls/Monitor.crt'
MANAGEMENT_TLS_KEY = '/etc/nvmesh/tls/Monitor.key'
MANAGEMENT_TLS_CA = '/etc/nvmesh/tls/ca_chain.crt'
MANAGEMENT_RETRY_LOGIN = 60
MANAGEMENT_MAX_REQUEST_RETRIES = 0
REST_SERVERS = None
TLS_CERT = None
TLS_KEY = None
TLS_CA = None
TLS_CLIENT_AUTH_REQUIRED = False
TLS_RUNTIME_DIR = '/var/run/nvmesh/tls/exporter'
CERT_AUTO_RELOAD = False
EXIT_ON_CERT_CHANGE = False
MEMORY_PROFILING = False
MEMORY_REPORT_INTERVAL = 60
MEMORY_TRACEMALLOC_LIMIT = 10
# source (i.e. /proc/nvmeibc/volumes/*/iostats.json) -> MetricParser object
SOURCE_CONF = {}
host = Host.instance(name=socket.gethostname())
cycle_count = 0
cache_size_metric : Optional[MetricWrapperBase] = None
cert_reload_counter : Optional[MetricWrapperBase]= None
cert_reload_timestamp : Optional[MetricWrapperBase] = None
loop_duration_metric : Optional[MetricWrapperBase] = None
loop_duration_max_metric : Optional[MetricWrapperBase] = None
loop_duration_max : float = 0.0

def load_internal_metrics():
    global cache_size_metric, cert_reload_counter, cert_reload_timestamp, loop_duration_metric, loop_duration_max_metric
    cache_size_metric = Gauge(name='nvmesh_exporter_cache_size', documentation="internal cache size", labelnames=['cache_name'])
    cert_reload_counter = Gauge(name='nvmesh_exporter_cert_reloads_total', documentation="total number of successful certificate reloads", labelnames=['cert_component'])
    cert_reload_timestamp = Gauge(name='nvmesh_exporter_cert_reload_timestamp', documentation="timestamp of last successful certificate reload", labelnames=['cert_component'])
    loop_duration_metric = Gauge(name='nvmesh_exporter_loop_duration_seconds', documentation="duration of current metrics collection loop iteration in seconds")
    loop_duration_max_metric = Gauge(name='nvmesh_exporter_loop_duration_max_seconds', documentation="maximum duration of any metrics collection loop iteration in seconds")
    Gauge(name='nvmesh_exporter_version', documentation='nvmesh exporter version', labelnames=['version']).labels(version=EXPORTER_VERSION).set(1)
    git_spec_info = get_path(f'{os.path.dirname(__file__)}/git_spec.info', 'git_spec.info')
    if not os.path.exists(git_spec_info):
        return

    global GIT_SPEC
    with open(git_spec_info, 'r') as fp:
        GIT_SPEC = dict(kv.split(':') for kv in fp.read().replace('\n', '').split())
        Gauge(name='nvmesh_exporter_git_spec', documentation='nvmesh exporter git spec', labelnames=GIT_SPEC.keys()).labels(**GIT_SPEC).set(1)


def print_version():
    print(f'NVMESH EXPORTER API VERSION: {EXPORTER_VERSION}')
    if GIT_SPEC:
        print(f'NVMESH DEPENDENCIES GIT SPEC:')
        for k, v in GIT_SPEC.items():
            print(f'\t{k.upper()}: {v}')


def bool_check(t_f):
    return t_f.upper() == "TRUE"

def get_default(arg_name, prefix='MONITOR_'):
    return host.nvmeshconf().get(prefix + arg_name, globals()[arg_name])

try:
    # Refresh .nvmesh.conf before loading arguments
    subprocess.call('/opt/nvmesh/bin/process_config_files')
except Exception as e:
    pass

parser = ArgumentParser()
parser.add_argument('--conf', default=get_default('METRICS_CONF_PATH'), help='metrics conf yaml')
parser.add_argument('--port', type=int, default=get_default('SERVER_PORT'), help='listener for prometheus exporter')
parser.add_argument('--interval', type=float, default=get_default('INTERVAL'), help='interval between repeats')
parser.add_argument('--del-threshold', type=int, default=get_default('DEL_THRESHOLD'), help='number of intervals the metric is missing before removing the metric')
parser.add_argument('--additional-labels', default=get_default('ADDITIONAL_LABELS'), nargs='+', help='globally configured labels to add to all metrics on the node. example: cluster_name:myCluster;rack_id:RackA')
parser.add_argument('-m', '--mgmt', default=get_default('REST_SERVERS'), help='management servers')
parser.add_argument('--mgmt-port', default=4000, help='management port')
parser.add_argument('--mgmt-use-tls', type=bool_check, default=get_default('MANAGEMENT_USE_TLS'), help='Use tls authentication')
parser.add_argument('--mgmt-cert', default=get_default('MANAGEMENT_TLS_CERT'), help='cert file for tls connection with management')
parser.add_argument('--mgmt-key', default=get_default('MANAGEMENT_TLS_KEY'), help='key file for tls connection with management')
parser.add_argument('--mgmt-ca', default=get_default('MANAGEMENT_TLS_CA'), help='ca file for tls connection with management')
parser.add_argument('--mgmt-retry-login', default=get_default('MANAGEMENT_RETRY_LOGIN'), help='retry management login wait in seconds')
parser.add_argument('--mgmt-max-request-retries', default=get_default('MANAGEMENT_MAX_REQUEST_RETRIES'), help='max number or retries to management requests')
parser.add_argument('--creds-file', default=get_default('MANAGEMENT_CREDS_FILE'), help='path to management creds authentication')
parser.add_argument('--exporter-cert', default=get_default('TLS_CERT'), help='cert file for tls connection with exporter')
parser.add_argument('--exporter-key', default=get_default('TLS_KEY'), help='key file for tls connection with exporter')
parser.add_argument('--exporter-ca', default=get_default('TLS_CA'), help='ca file for tls connection with exporter')
parser.add_argument('--exporter-client-auth-required', type=bool_check, default=get_default('TLS_CLIENT_AUTH_REQUIRED'), help='Enforce mutual TLS with exporter')
parser.add_argument('--tls-runtime-dir', default=get_default('TLS_RUNTIME_DIR'), help='Runtime directory for certificate copies (for observability)')
parser.add_argument('--cert-auto-reload', type=bool_check, default=get_default('CERT_AUTO_RELOAD'), help='Auto reload certs if file updated')
parser.add_argument('--exit-on-cert-change', type=bool_check, default=get_default('EXIT_ON_CERT_CHANGE'), help='Exit on cert change (for k8 pod) - implies cert auto-reload')
parser.add_argument('-u', '--user', default=get_default('MANAGEMENT_USER'), help='management user log in')
parser.add_argument('-V', '--version', action='store_true', help='NVMesh exporter version')
parser.add_argument("-L", "--loglevel", help='Logging-level for stderr', default=get_default('LOGGING_LEVEL'))
parser.add_argument("--logfile", help='Log file', default=get_default('LOGGING_FILE'))
# Memory profiling arguments
parser.add_argument('--memory-profiling', type=bool_check, default=get_default('MEMORY_PROFILING'), help='Enable memory profiling and periodic reporting')
parser.add_argument('--memory-report-interval', type=int, default=get_default('MEMORY_REPORT_INTERVAL'), help='Memory report interval in seconds (default: 60)')
parser.add_argument('--memory-tracemalloc-limit', type=int, default=get_default('MEMORY_TRACEMALLOC_LIMIT'), help='Number of top memory allocations to report (default: 10)')
parsed_args = parser.parse_args()

# Set runtime certificate directories based on parsed argument
EXPORTER_TLS_RUNTIME_DIR = os.path.join(parsed_args.tls_runtime_dir, 'prometheus')
MANAGEMENT_TLS_RUNTIME_DIR = os.path.join(parsed_args.tls_runtime_dir, 'management')

load_internal_metrics()
if parsed_args.version:
    print_version()
    sys.exit(0)

logger = logging.getLogger('.'.join([os.path.basename(__file__), host.name]))
logging.basicConfig(filename=parsed_args.logfile, filemode='w', level=parsed_args.loglevel, format='%(asctime)s: %(levelname)s: [%(thread)d] %(message)s', datefmt='%Y-%m-%d %H:%M:%S')

if parsed_args.mgmt:
    mgmt_hosts = [host_name(m.split(':')[0]) for m in parsed_args.mgmt.split(',')]
    mgmt = Manager.instance(endpoints=mgmt_hosts)
else:
    mgmt_hosts = [host.name]
    mgmt = Manager.instance(endpoints=[host.name])

mgmt._rest_endpoints = mgmt_hosts
AGG_CNTR = {}
AGG_LOCK = Lock()

profiler = None
profiler_enabled = parsed_args.memory_profiling  # Global flag for runtime control of memory profiler
exit_on_cert_change = parsed_args.exit_on_cert_change
cert_auto_reload = parsed_args.cert_auto_reload or exit_on_cert_change

# Certificate reload infrastructure (signal-based + optional auto-reload)
exporter_certs = [parsed_args.exporter_cert, parsed_args.exporter_key, parsed_args.exporter_ca]
mgmt_certs = [parsed_args.mgmt_cert, parsed_args.mgmt_key, parsed_args.mgmt_ca]
has_exporter_certs = any(exporter_certs)
has_mgmt_certs = any(mgmt_certs) if parsed_args.mgmt_use_tls else False
cert_mtimes = {}  # Track certificate file modification times for auto-reload
cert_reload_lock = threading.RLock()
loaded_exporter_cert: Optional[str] = os.path.join(EXPORTER_TLS_RUNTIME_DIR, 'cert.crt') if parsed_args.exporter_cert else None
loaded_exporter_key: Optional[str] = os.path.join(EXPORTER_TLS_RUNTIME_DIR, 'key.key') if parsed_args.exporter_key else None
loaded_exporter_ca: Optional[str] = os.path.join(EXPORTER_TLS_RUNTIME_DIR, 'ca.crt') if parsed_args.exporter_ca else None
loaded_mgmt_cert: Optional[str] = os.path.join(MANAGEMENT_TLS_RUNTIME_DIR, 'cert.crt') if parsed_args.mgmt_cert else None
loaded_mgmt_key: Optional[str] = os.path.join(MANAGEMENT_TLS_RUNTIME_DIR, 'key.key') if parsed_args.mgmt_key else None
loaded_mgmt_ca: Optional[str] = os.path.join(MANAGEMENT_TLS_RUNTIME_DIR, 'ca.crt') if parsed_args.mgmt_ca else None

prometheus_server = None  # Will hold the server object for graceful shutdown
prometheus_thread = None  # Will hold the server thread
start_time = time.time()  # Track process start time for uptime calculation

def is_port_available(port: int) -> bool:
    """Check if a port is available for binding without SO_REUSEADDR."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            # Don't use SO_REUSEADDR to match prometheus_client behavior
            sock.bind(('', port))
            return True
    except OSError:
        return False

def check_port_usage(port: int) -> str:
    """Check what's using a specific port."""
    try:
        result = subprocess.run(['netstat', '-tlnp'], capture_output=True, text=True, timeout=5)
        port_lines = [line for line in result.stdout.split('\n') if f':{port} ' in line]
        return f"Port {port} usage: " + '; '.join(port_lines) if port_lines else f"Port {port} not found in netstat output"
    except Exception as e:
        return f"Failed to check port usage: {e}"

class Aggregate(object):
    def __init__(self, name, conf, general_labels):
        self.aggregate_by = conf['aggregate_by']
        self.sum_by = conf.get('sum_by')
        self.query_labels = conf.get('query_labels', {})
        self.query_labels_list = conf.get('query_labels_list', {})
        self.ignore = conf.get('ignore', [])
        labelnames = [l for l in general_labels + list(self.query_labels.keys()) + list(self.aggregate_by.keys()) + self.query_labels_list.get('names', []) if l not in self.ignore]
        try:
            self.metric = Gauge(name=name, documentation=conf.get('help'), labelnames=labelnames)
        except ValueError:
            self.metric = REGISTRY._names_to_collectors[name]


class MetricData(object):
    _instances = {}
    DEL_THRESHOLD = parsed_args.del_threshold

    def __new__(cls, metric, label_values):
        key = (metric, label_values)
        if key in cls._instances:
            return cls._instances[key]

        instance = super().__new__(cls)
        cls._instances[key] = instance
        return instance

    def __init__(self, metric, label_values):
        self.metric = metric
        self.label_values = label_values
        self.del_threshold = self.DEL_THRESHOLD


class ParsingStrategy(object):
    def __init__(self, metrics_conf: Dict[str, Any], parser_labels: Optional[List[str]] = None):
        self.metric_prefix = metrics_conf.get('metric_prefix', '')

        # General configuration for all metrics in current proc
        self.glob_re, glob_labels = self.re_to_labels_data(metrics_conf.get('glob_labels_re'))
        self.pattern_re, pattern_labels = self.re_to_labels_data(metrics_conf.get('pattern_labels_re'))
        self.query_labels = metrics_conf.get('query_labels', {})
        self.query_labels_list = metrics_conf.get('query_labels_list', {})
        self.transform = metrics_conf.get('transform')

        general_labels = list(set((parser_labels or []) + list(self.additional_labels().keys()) + glob_labels + pattern_labels + list(self.query_labels.keys()) + self.query_labels_list.get('names', [])))
        self.metrics : Dict[Union[re.Pattern, List], MetricWrapperBase] = {}
        for m_name, m_conf in metrics_conf.get('metrics', {}).items():
            # METRIC specific configurations
            pattern_re, pattern_labels = self.re_to_labels_data(m_conf.get('pattern_labels_re'))
            metric_name = f'{self.metric_prefix}_{m_name}'


            # TODO: in the future, use m_conf.get('type') to generate other types of metrics
            try:
                metric = Gauge(name=metric_name, documentation=m_conf.get('help'), labelnames=general_labels + pattern_labels)
            except ValueError:
                metric = REGISTRY._names_to_collectors[metric_name]

            self.metrics[pattern_re or metric_name] = metric

        self.aggregates = {}
        for a_name, a_conf in metrics_conf.get('aggregates', {}).items():
            # AGGREGATE specific configurations
            aggregate_name = f'{self.metric_prefix}_{a_name}'
            self.aggregates[aggregate_name] = Aggregate(aggregate_name, a_conf, general_labels)

        general_conf = {}
        for k, v in metrics_conf.items():
            if k not in ['metrics', 'aggregates', 'collections', 'query_labels', 'query_labels_list', 'transform']:
                general_conf[k] = v

        self.collections = {}
        for c_name, c_conf in metrics_conf.get('collections', {}).items():
            # COLLECTION specific configurations
            g_conf = general_conf.copy()
            g_conf.update(c_conf)
            self.collections[c_name] = type(self)(g_conf, general_labels)

    @staticmethod
    def re_to_labels_data(labels_re: Optional[str]) -> Tuple[Optional[re.Pattern], List]:
        """ Take a metric name and return a tuple of re.compile regex and labels """
        if not labels_re:
            return None, []

        compiled_re = re.compile(labels_re)
        try:
            labels = list(compiled_re.groupindex.keys())
        except Exception:
            labels = []

        return compiled_re, labels

    @classmethod
    def additional_labels(cls):
        if not parsed_args.additional_labels:
            return {}

        additional_labels_dict = {}
        for additional_label in parsed_args.additional_labels.split(';'):
            k, v = additional_label.split(':')
            additional_labels_dict[k] = v
        return additional_labels_dict


class MetricParser(object):
    ACTIVE_METRICS_DATA : Set[MetricData] = set()
    _version_delimiter = '|'
    _default_version = '0'
    _version_map = {}
    _parser_labels = ['host']

    def __init__(self, source, versioned_metrics_conf: Dict[str, Any]):
        self.source = source
        self.version2metric : Dict[str, 'ParsingStrategy'] = {}
        for version, metrics_conf in versioned_metrics_conf.items():
            self.version2metric[version] = ParsingStrategy(metrics_conf, self._parser_labels)

    @abstractmethod
    def fetch_metrics_dict(self, path: str) -> Dict[str, Any]:
        pass

    @classmethod
    def add_active_metric(cls, metric: MetricWrapperBase, label_values: Tuple):
        cls.ACTIVE_METRICS_DATA.add(MetricData(metric, label_values))

    @classmethod
    def decrease_metric_threshold(cls):
        for metric_data in cls.ACTIVE_METRICS_DATA:
            metric_data.del_threshold -= 1

    @classmethod
    def remove_inactive_metrics(cls):
        non_active_metrics = set()
        removed_count = 0

        for metric_data in cls.ACTIVE_METRICS_DATA:
            if metric_data.del_threshold <= 0:
                try:
                    # Record before removal for debugging
                    metric_name = getattr(metric_data.metric, '_name', 'unknown')

                    # Remove from Prometheus registry
                    metric_data.metric.remove(*metric_data.label_values)
                    removed_count += 1

                    # Log significant removals for debugging
                    if removed_count <= 5:  # Log first 5 removals
                        logger.debug(f"Removed metric: {metric_name} with labels {metric_data.label_values}")

                except Exception as e:
                    logger.error(f"Error removing metric {getattr(metric_data.metric, '_name', 'unknown')}: {e}")

                non_active_metrics.add(metric_data)

        # Clean up the instances cache to prevent memory leak
        for metric_data in non_active_metrics:
            key = (metric_data.metric, metric_data.label_values)
            MetricData._instances.pop(key, None)

        cls.ACTIVE_METRICS_DATA -= non_active_metrics

        if removed_count > 0:
            logger.debug(f"Removed {removed_count} inactive metrics from Prometheus registry")

    def get_doc_version(self, raw_content: Any) -> str:
        return self._default_version

    def get_version(self, raw_content: Any):
        version = self.get_doc_version(raw_content)
        if not self._version_map.get(version):
            target = Version(version)
            versions = sorted([Version(v) for v in self.version2metric.keys()], reverse=True)
            for v in versions:
                try:
                    if v <= target:
                        logger.debug(f'source {self.source} - target version: {version}. Compatible version: {v}.')
                        self._version_map[version] = str(v)
                        break
                except InvalidVersion:
                    logger.error(f'Unsupported version {v} for source {self.source}')
                    continue

        return self._version_map.get(version)

    def get_raw_data(self, path: str) -> Tuple[Dict[str, Any], str]:
        raw_content = self.fetch_metrics_dict(path)
        version = self.get_version(raw_content)
        return raw_content, version

    def parser_labels(self):
        return {'host': host.name}

    def __repr__(self):
        return str(self.__dict__)


class CmdParser(MetricParser):
    sp_run_kwargs = {'shell': True, 'check': True, 'capture_output': True, 'text': True}

    @classmethod
    def run_cmd(cls, cmd: str) -> str:
        return subprocess.run(cmd, **cls.sp_run_kwargs).stdout

    def fetch_metrics_dict(self, path: str) -> Dict[str, Any]:
        return json.loads(self.run_cmd(path))


class UmRpcParser(CmdParser):
    RPC_CMD = "nvmeshum_spdk_rpc"
    is_rpc_available = shutil.which(RPC_CMD) is not None

    def fetch_metrics_dict(self, path: str) -> Dict[str, Any]:
        return super().fetch_metrics_dict(f'{self.RPC_CMD} {path}')

    @classmethod
    @abstractmethod
    def build_rpc_cmd(cls, source, elem) -> str:
        pass


class UmRpcOperationParser(UmRpcParser):
    def get_iterables(self, source) -> List[str]:
        _, ent_type, _ = source.split('@')
        bdevs_res = self.fetch_metrics_dict(f'nvmesh_list_class_objects --class {ent_type}')
        return [e['name'] for e in bdevs_res["class objects"]]

    @classmethod
    def build_rpc_cmd(cls, source, elem) -> str:
        _, ent_type, op_type = source.split('@')
        return f'nvmesh_rpc_operation --class {ent_type} --name {elem} --op-type {op_type} --sum-only'


class UmRpcLvolParser(UmRpcParser):
    def get_iterables(self, source) -> List[str]:
        lvol_bdevs_res = self.fetch_metrics_dict('bdev_lvol_get_lvols')
        return [e["lvs"]["name"].strip('_lvs') for e in lvol_bdevs_res]

    @classmethod
    def build_rpc_cmd(cls, source, elem) -> str:
        return f'bdev_get_iostat -b {elem} | sed "s/written/write/g; s/unmapped\|unmap/trim/g"'


class RestParser(MetricParser):
    """
    Parser for REST API endpoints.

    This parser can be configured to use either:
    1. A global management connection (default behavior)
    2. A single specific REST endpoint (when endpoint parameter is provided)

    When endpoint is provided, the parser will create a separate connection
    to that specific endpoint instead of using the global management connection.
    This allows for more granular control over which endpoints are used for
    different metrics.
    """
    _api_version = MetricParser._default_version
    _connection_cache : Dict[str, Union['ConnectionManager', float]] = {}
    _cache_lock = RLock()
    _parser_labels = ['mgmt_host']
    infra_conf.root.connection_manager.max_http_retries = parsed_args.mgmt_max_request_retries

    def __init__(self, source, versioned_metrics_conf: Dict[str, Any], endpoint: Optional[str] = None):
        self._endpoint = endpoint
        super().__init__(source, versioned_metrics_conf=versioned_metrics_conf)

    def connection(self):
        """Get or create connection for this instance using the connection cache"""
        endpoint_key = self._endpoint or f"{','.join(sorted(mgmt_hosts))}"
        if endpoint_key not in self._connection_cache or (isinstance(self._connection_cache.get(endpoint_key), float) and time.time() >= self._connection_cache.get(endpoint_key)):
            with self._cache_lock:
                if endpoint_key not in self._connection_cache or (isinstance(self._connection_cache.get(endpoint_key), float) and time.time() >= self._connection_cache.get(endpoint_key)):
                    logger.debug(f'Acquiring mgmt connection for endpoint key: {endpoint_key}')
                    try:
                        conn_mgmt = Manager.instance(endpoints=endpoint_key.split(','))
                        connection = conn_mgmt.connect(parsed_args.user, use_tls=parsed_args.mgmt_use_tls, cert=loaded_mgmt_cert, key=loaded_mgmt_key, ca=loaded_mgmt_ca, creds_file=parsed_args.creds_file)
                        mgmt_servers = [c for c in connection.managementServers if urlparse(c).hostname in endpoint_key.split(',')]
                        connection.setManagementServers(mgmt_servers)
                        self._connection_cache[endpoint_key] = connection
                        #JJW: Does global API version make sense with multiple endpoints?
                        self._api_version = conn_mgmt.api_version
                        logger.info(f'Connection acquired successfully for endpoint key: {endpoint_key}! API Version: {self._api_version}')
                    except Exception as e:
                        next_retry = time.time() + parsed_args.mgmt_retry_login
                        self._connection_cache[endpoint_key] = next_retry
                        raise ConnectionError(f'Unable to acquire connection for endpoint key: {endpoint_key} - {repr(e)}. Retry login after {time.ctime(next_retry)}')
                elif isinstance(self._connection_cache.get(endpoint_key), float):
                    raise ConnectionError(f'Reconnect pending in {self._connection_cache[endpoint_key]-time.time()}.')
        elif isinstance(self._connection_cache.get(endpoint_key), float) and time.time() < self._connection_cache.get(endpoint_key):
            raise ConnectionError(f'Retry login after: {time.ctime(self._connection_cache[endpoint_key])}')

        logger.debug(f'connection(): returning connection for endpoint key: {endpoint_key}: {self._connection_cache[endpoint_key]}')
        return self._connection_cache[endpoint_key]

    def parser_labels(self):
        try:
            mgmt_host = self._endpoint or re.match(r'^https?://([^/:]+)', self.connection().managementServer).group(1)
        except AttributeError:
            mgmt_host = None
        return {'mgmt_host': mgmt_host}

    def fetch_metrics_dict(self, path: str) -> Dict[str, Any]:
        """Fetch metrics using this specific RestParser instance's connection"""
        err, raw_content = self.connection().get(path)
        assert not err, f'Failed to fetch from REST {path} - {err}'
        return {'total': raw_content} if isinstance(raw_content, (int, float)) else raw_content

    def get_doc_version(self, raw_content: Any) -> str:
        return self._api_version


class ProcParser(MetricParser):
    _version_query = 'format_version'

    def get_doc_version(self, raw_content: Any) -> str:
        return str(jmespath.search(ProcParser._version_query, raw_content) or self._default_version)


class JsonProcParser(ProcParser):
    def fetch_metrics_dict(self, path: str) -> Dict[str, Any]:
        with open(path, 'r') as fp:
            return json.loads(fp.read())


class KeyValueProcParser(ProcParser):
    def fetch_metrics_dict(self, path: str) -> Dict[str, Any]:
        with open(path, 'r') as fp:
            # need to support floats as well?
            key_value_list = [l.split(':', maxsplit=1) for l in fp.read().replace(' ', '').splitlines()]
            return {e[0]: int(e[1]) for e in key_value_list if e[1].isnumeric()}


class MemoryProfiler:
    """Memory profiling utilities for tracking memory usage and potential leaks"""
    tracked_caches = {
        'MetricData._instances': MetricData._instances,
        'MetricParser.ACTIVE_METRICS_DATA': MetricParser.ACTIVE_METRICS_DATA,
        'RestParser._connection_cache': RestParser._connection_cache,
        'SOURCE_CONF': SOURCE_CONF
    }

    def __init__(self, enabled: bool = True, report_interval: int = 60, tracemalloc_limit: int = 10):
        self.enabled = enabled
        self.report_interval = report_interval
        self.tracemalloc_limit = tracemalloc_limit
        self.last_report_time = time.time()
        self.baseline_snapshot = None
        self.cycle_snapshots = []
        self.process = psutil.Process()

        if self.enabled:
            logger.info(f"[MEMORY] Memory profiling enabled - report interval: {report_interval}s, tracemalloc: enabled")
            # Always start tracemalloc when profiling is enabled
            tracemalloc.start()
            logger.info("[MEMORY] Tracemalloc memory tracking started")

    def get_memory_info(self) -> Dict[str, Any]:
        """Get current memory usage information"""
        if not self.enabled:
            return {}

        try:
            memory_info = self.process.memory_info()
            memory_percent = self.process.memory_percent()

            info = {
                'rss_mb': round(memory_info.rss / 1024 / 1024, 2),
                'vms_mb': round(memory_info.vms / 1024 / 1024, 2),
                'memory_percent': round(memory_percent, 2),
                'timestamp': time.time()
            }

            # Add system memory info
            system_memory = psutil.virtual_memory()
            info.update({
                'system_total_mb': round(system_memory.total / 1024 / 1024, 2),
                'system_available_mb': round(system_memory.available / 1024 / 1024, 2),
                'system_percent': system_memory.percent
            })

            # Add tracemalloc info (always enabled when profiling is on)
            if tracemalloc.is_tracing():
                current_size, peak_size = tracemalloc.get_traced_memory()
                info.update({
                    'tracemalloc_current_mb': round(current_size / 1024 / 1024, 2),
                    'tracemalloc_peak_mb': round(peak_size / 1024 / 1024, 2)
                })

            return info
        except Exception as e:
            logger.error(f"[MEMORY] Error getting memory info: {e}")
            return {}

    def take_snapshot(self, label: str = "") -> Optional[Any]:
        """Take a memory snapshot using tracemalloc"""
        if not self.enabled or not tracemalloc.is_tracing():
            return None

        try:
            snapshot = tracemalloc.take_snapshot()
            snapshot.label = label
            snapshot.timestamp = time.time()
            return snapshot
        except Exception as e:
            logger.error(f"[MEMORY] Error taking memory snapshot: {e}")
            return None

    def set_baseline(self):
        """Set the baseline memory snapshot"""
        if not self.enabled:
            return

        self.baseline_snapshot = self.take_snapshot("baseline")
        if self.baseline_snapshot:
            memory_info = self.get_memory_info()
            logger.info(f"[MEMORY] Memory baseline set - RSS: {memory_info.get('rss_mb', 0)}MB")

    def report_memory_usage(self, force: bool = False):
        """Report current memory usage"""
        if not self.enabled:
            return

        current_time = time.time()
        if not force and (current_time - self.last_report_time) < self.report_interval:
            return

        memory_info = self.get_memory_info()
        if memory_info:
            logger.info(f"[MEMORY] Memory Report - RSS: {memory_info['rss_mb']}MB, VMS: {memory_info['vms_mb']}MB, "
                       f"Process%: {memory_info['memory_percent']}%, System%: {memory_info['system_percent']}%")

            if 'tracemalloc_current_mb' in memory_info:
                logger.info(f"[MEMORY] Tracemalloc - Current: {memory_info['tracemalloc_current_mb']}MB, "
                           f"Peak: {memory_info['tracemalloc_peak_mb']}MB")

        self.last_report_time = current_time

    def report_top_allocations(self):
        """Report top memory allocations using tracemalloc"""
        if not self.enabled or not tracemalloc.is_tracing():
            return

        try:
            snapshot = tracemalloc.take_snapshot()
            top_stats = snapshot.statistics('lineno')

            logger.info(f"[MEMORY] Top {self.tracemalloc_limit} memory allocations:")
            for index, stat in enumerate(top_stats[:self.tracemalloc_limit], 1):
                logger.info(f"[MEMORY] #{index}: {stat}")

        except Exception as e:
            logger.error(f"[MEMORY] Error reporting top allocations: {e}")

    def detect_memory_leaks(self, current_snapshot):
        """Compare current snapshot with baseline to detect potential leaks"""
        if not self.enabled or not self.baseline_snapshot or not current_snapshot:
            return

        try:
            top_stats = current_snapshot.compare_to(self.baseline_snapshot, 'lineno')

            # Filter for significant increases (>1MB)
            significant_increases = [stat for stat in top_stats if stat.size_diff > 1024 * 1024]

            if significant_increases:
                logger.warning(f"[MEMORY] Potential memory leaks detected ({len(significant_increases)} locations):")
                for stat in significant_increases[:5]:  # Top 5 increases
                    logger.warning(f"[MEMORY]   {stat}")

        except Exception as e:
            logger.error(f"[MEMORY] Error detecting memory leaks: {e}")

    def report_cache_sizes(self):
        """Report sizes of known caches that might leak"""
        if not self.enabled:
            return

        try:
            cache_info = {k: len(v) for k, v in MemoryProfiler.tracked_caches.items()}
            # Try to get version maps from all parsers
            total_version_maps = 0
            for parser in SOURCE_CONF.values():
                if hasattr(parser, '_version_map'):
                    total_version_maps += len(parser._version_map)
            cache_info['Total _version_maps'] = total_version_maps

            logger.info(f"[MEMORY] Cache sizes: {cache_info}")

            # Report prometheus registry size
            try:
                from prometheus_client.registry import REGISTRY
                registry_size = len(REGISTRY._names_to_collectors)
                total_metrics = sum(len(getattr(collector, '_metrics', {})) for collector in REGISTRY._names_to_collectors.values())
                logger.info(f"[MEMORY] Prometheus Registry - Collectors: {registry_size}, Total Metric Instances: {total_metrics}")

                # Calculate estimated memory from Prometheus metrics
                estimated_prometheus_mb = total_metrics * 1.5 / 1024  # Rough estimate: 1.5KB per metric instance
                logger.info(f"[MEMORY] Estimated Prometheus Memory Usage: {estimated_prometheus_mb:.1f}MB")

            except Exception as e:
                logger.warning(f"[MEMORY] Could not get prometheus registry info: {e}")

            # Force garbage collection and report
            import gc
            collected = gc.collect()
            logger.info(f"[MEMORY] Garbage collection freed {collected} objects")

            # Report RSS vs tracemalloc gap
            memory_info = self.get_memory_info()
            if memory_info:
                rss_mb = memory_info.get('rss_mb', 0)
                tracemalloc_mb = memory_info.get('tracemalloc_current_mb', 0)
                gap_mb = rss_mb - tracemalloc_mb
                logger.warning(f"[MEMORY] Memory Gap Analysis - RSS: {rss_mb}MB, Tracemalloc: {tracemalloc_mb}MB, "
                                   f"Untracked: {gap_mb}MB ({gap_mb/rss_mb*100:.1f}%)")

        except Exception as e:
            logger.error(f"[MEMORY] Error reporting cache sizes: {e}")

    @classmethod
    def free_memory(cls):
        # Force aggressive garbage collection
        import gc
        for _ in range(3):  # Multiple passes
            collected = gc.collect()

        # Try to force Python to return memory to OS (doesn't always work)
        try:
            # This is a hack that sometimes helps
            import ctypes
            libc = ctypes.CDLL("libc.so.6")
            libc.malloc_trim(0)
        except:
            pass  # Not available on all systems

    def test_memory_freeing(self):
        """Test if memory is actually freed when metrics are removed"""
        if not self.enabled:
            return

        try:
            # Get memory before cleanup
            before_memory = self.get_memory_info()
            before_rss = before_memory.get('rss_mb', 0)

            MemoryProfiler.free_memory()

            # Get memory after cleanup
            after_memory = self.get_memory_info()
            after_rss = after_memory.get('rss_mb', 0)

            memory_freed = before_rss - after_rss
            logger.info(f"[MEMORY] Memory cleanup test - Before: {before_rss}MB, After: {after_rss}MB, "
                           f"Freed: {memory_freed:.2f}MB")

            if memory_freed < 0.1:
                logger.info("[MEMORY] ℹ️  RSS memory was not returned to OS (normal Python behavior)")
                logger.info("[MEMORY]    Python keeps freed memory for reuse - not a leak!")
            else:
                logger.info(f"[MEMORY] ✅ Successfully freed {memory_freed:.2f}MB of memory")

        except Exception as e:
            logger.error(f"[MEMORY] Error testing memory freeing: {e}")

    def check_memory_stability(self, cycle_count: int):
        """Check if RSS memory stabilizes (indicating reuse rather than growth)"""
        if not self.enabled:
            return

        try:
            memory_info = self.get_memory_info()
            current_rss = memory_info.get('rss_mb', 0)

            # Store last 10 RSS measurements for trend analysis
            if not hasattr(self, '_rss_history'):
                self._rss_history = []

            self._rss_history.append(current_rss)
            if len(self._rss_history) > 10:
                self._rss_history.pop(0)

            # Analyze trend every 10 cycles
            if cycle_count % 10 == 0 and len(self._rss_history) >= 5:
                recent_rss = self._rss_history[-5:]  # Last 5 measurements
                min_rss = min(recent_rss)
                max_rss = max(recent_rss)
                rss_variation = max_rss - min_rss

                if rss_variation < 5:  # Less than 5MB variation
                    logger.info(f"[MEMORY] ✅ Memory appears stable: {min_rss:.1f}-{max_rss:.1f}MB range (variation: {rss_variation:.1f}MB)")
                    logger.info("[MEMORY]    This suggests memory reuse rather than continuous growth")
                elif rss_variation < 20:
                    logger.info(f"[MEMORY] 📊 Memory variation: {rss_variation:.1f}MB over last 5 reports - monitoring...")
                else:
                    logger.warning(f"[MEMORY] ⚠️  High memory variation: {rss_variation:.1f}MB - possible continued growth")

        except Exception as e:
            logger.error(f"[MEMORY] Error checking memory stability: {e}")

    def detect_memory_leak_via_recreate_test(self, cycle_count: int, active_metrics_count: int):
        """Detect memory leaks by monitoring RSS when recreating similar workloads"""
        if not self.enabled:
            return

        try:
            memory_info = self.get_memory_info()
            current_rss = memory_info.get('rss_mb', 0)

            # Track RSS at different metric counts
            if not hasattr(self, '_workload_memory_map'):
                self._workload_memory_map = {}  # metric_count -> [rss_measurements]

            # Group metrics count into bins (to handle minor variations)
            metric_bin = (active_metrics_count // 1000) * 1000  # Round to nearest 1000

            if metric_bin not in self._workload_memory_map:
                self._workload_memory_map[metric_bin] = []

            self._workload_memory_map[metric_bin].append(current_rss)

            # Keep only last 5 measurements per workload level
            if len(self._workload_memory_map[metric_bin]) > 5:
                self._workload_memory_map[metric_bin].pop(0)

            # Analysis: Check if RSS increases for same workload size
            if len(self._workload_memory_map[metric_bin]) >= 3:
                measurements = self._workload_memory_map[metric_bin]
                first_measurement = measurements[0]
                recent_measurement = measurements[-1]
                growth = recent_measurement - first_measurement

                # Log analysis every 20 cycles
                if cycle_count % 20 == 0:
                    if growth > 20:  # More than 20MB growth for same workload
                        logger.error(f"[MEMORY] 🚨 MEMORY LEAK DETECTED!")
                        logger.error(f"[MEMORY]    Workload: ~{metric_bin} metrics")
                        logger.error(f"[MEMORY]    RSS growth: {first_measurement:.1f}MB → {recent_measurement:.1f}MB (+{growth:.1f}MB)")
                        logger.error(f"[MEMORY]    Same workload should reuse memory, not grow indefinitely!")
                    elif growth > 5:
                        logger.warning(f"[MEMORY] ⚠️  Potential memory leak detected")
                        logger.warning(f"[MEMORY]    Workload: ~{metric_bin} metrics, RSS growth: +{growth:.1f}MB over {len(measurements)} cycles")
                    else:
                        logger.info(f"[MEMORY] ✅ Memory reuse working correctly for ~{metric_bin} metrics (growth: +{growth:.1f}MB)")

            # Report workload summary periodically
            if cycle_count % 50 == 0 and self._workload_memory_map:
                logger.info("[MEMORY] 📊 Workload Memory Analysis:")
                for workload, measurements in sorted(self._workload_memory_map.items()):
                    if measurements:
                        avg_rss = sum(measurements) / len(measurements)
                        min_rss = min(measurements)
                        max_rss = max(measurements)
                        logger.info(f"[MEMORY]    ~{workload} metrics: {min_rss:.1f}-{max_rss:.1f}MB (avg: {avg_rss:.1f}MB, {len(measurements)} samples)")

        except Exception as e:
            logger.error(f"[MEMORY] Error in recreate test: {e}")


def _initialize_memory_profiler():
    """Initialize memory profiler if needed."""
    global profiler
    if not profiler:
        profiler = MemoryProfiler(
            report_interval=parsed_args.memory_report_interval,
            tracemalloc_limit=parsed_args.memory_tracemalloc_limit,
        )
        profiler.set_baseline()
        profiler.report_memory_usage(force=True)


def _run_memory_profiling_reports():
    """Run memory profiling reports for current cycle."""
    global cycle_count
    cycle_end_snapshot = profiler.take_snapshot(f"cycle_{cycle_count}_end")
    profiler.report_memory_usage()
    profiler.check_memory_stability(cycle_count)

    current_metrics_count = len(MetricParser.ACTIVE_METRICS_DATA)
    profiler.detect_memory_leak_via_recreate_test(cycle_count, current_metrics_count)

    if cycle_count % 10 == 0:
        profiler.report_cache_sizes()
        profiler.report_top_allocations()

    if cycle_count % 60 == 0:
        profiler.detect_memory_leaks(cycle_end_snapshot)
        profiler.test_memory_freeing()


def _check_and_reload_certificates():
    """Check for certificate changes and reload if needed (only when auto-reload is enabled)."""
    if not cert_auto_reload:
        return  # Auto-reload is disabled

    try:
        if has_exporter_certs and check_certificate_changes(exporter_certs):
            if exit_on_cert_change:
                logger.warning("Exporter certificates changed - exiting process")
                sys.exit(0)
            logger.info("Auto-reload: Exporter certificates changed - performing full server restart")
            if not reload_exporter_certificates():
                logger.error("Failed to reload exporter certificates")

        if has_mgmt_certs and check_certificate_changes(mgmt_certs):
            if exit_on_cert_change:
                logger.warning("Management certificates changed - exiting process")
                sys.exit(0)
            logger.info("Auto-reload: Management certificates changed - clearing connection cache")
            if reload_management_certificates():
                logger.error("Failed to reload management certificates")
    except Exception as e:
        logger.error(f"Error checking certificates: {e}")


def _update_cache_metrics():
    """Update cache size metrics."""
    for cache_name, cache in MemoryProfiler.tracked_caches.items():
        cache_size_metric.labels(cache_name=cache_name).set(len(cache))


def _record_timing(start_time):
    """Record loop timing metrics."""
    global loop_duration_max
    duration = time.time() - start_time
    if loop_duration_metric:
        loop_duration_metric.set(duration)
    if loop_duration_max_metric and duration > loop_duration_max:
        loop_duration_max = duration
        loop_duration_max_metric.set(loop_duration_max)


def instrumented_loop(enable_timing=True, enable_gc=True, gc_interval=60):
    """
    Decorator to add comprehensive instrumentation to loop functions.

    Features:
    - Timing metrics (tracks current and max duration)
    - Memory profiling (when enabled via runtime flag)
    - Optional certificate auto-reload checks (when enabled via SIGUSR2)
    - Cache size monitoring
    - Periodic garbage collection
    - Cycle counting

    Args:
        enable_timing: Enable loop duration timing metrics (default: True)
        enable_gc: Enable periodic garbage collection (default: True)
        gc_interval: Cycles between GC runs (default: 60)
    """
    def decorator(func):
        def wrapper(*args, **kwargs):
            global profiler_enabled, cycle_count, profiler

            start_time = time.time() if enable_timing else None

            try:
                # Initialize memory profiler if enabled
                if profiler_enabled:
                    _initialize_memory_profiler()

                # Execute the core function
                result = func(*args, **kwargs)

                # Post-execution instrumentation
                _update_cache_metrics()

                _check_and_reload_certificates()

                if profiler_enabled:
                    _run_memory_profiling_reports()

                if enable_gc and cycle_count % gc_interval == 0 and not profiler:
                    MemoryProfiler.free_memory()

                cycle_count += 1

                return result

            finally:
                # Always record timing (even on exception)
                if enable_timing and start_time is not None:
                    _record_timing(start_time)

        return wrapper
    return decorator

def check_certificate_changes(certs_list: List[str]) -> bool:
    """Check which certificate files have changed.

    Returns:
        has_changed: bool indicating if certificates have changed
    """
    has_changed = False
    for filepath in certs_list:
        if filepath and os.path.exists(filepath):
            current_mtime = os.path.getmtime(filepath)
            if cert_mtimes.get(filepath) != current_mtime:
                cert_mtimes[filepath] = current_mtime
                logger.info(f"Certificate file change detected: {filepath}")
                has_changed = True

    return has_changed

def copy_cert_files(source_cert: Optional[str], source_key: Optional[str], source_ca: Optional[str],
                    dest_dir: str) -> Tuple[Optional[str], Optional[str], Optional[str]]:
    """Copy certificate files to runtime directory for observability.

    Args:
        source_cert: Path to source certificate file
        source_key: Path to source key file
        source_ca: Path to source CA file
        dest_dir: Destination directory for copied certificates

    Returns:
        Tuple of (dest_cert_path, dest_key_path, dest_ca_path) - paths to copied files
        Returns None for any file that doesn't exist or couldn't be copied
    """
    try:
        os.makedirs(dest_dir, exist_ok=True)
        logger.info(f"Created/verified runtime certificate directory: {dest_dir}")
    except Exception as e:
        logger.error(f"Failed to create certificate directory {dest_dir}: {e}")
        return None, None, None

    dest_cert = None
    dest_key = None
    dest_ca = None

    try:
        if source_cert and os.path.exists(source_cert):
            dest_cert = os.path.join(dest_dir, 'cert.crt')
            # Remove existing read-only file if present
            if os.path.exists(dest_cert):
                os.remove(dest_cert)
            shutil.copy2(source_cert, dest_cert)
            os.chmod(dest_cert, 0o444)  # Read-only for all
            logger.info(f"Copied certificate: {source_cert} -> {dest_cert} (read-only)")

        if source_key and os.path.exists(source_key):
            dest_key = os.path.join(dest_dir, 'key.key')
            # Remove existing read-only file if present
            if os.path.exists(dest_key):
                os.remove(dest_key)
            shutil.copy2(source_key, dest_key)
            os.chmod(dest_key, 0o400)  # Read-only for owner only (private key)
            logger.info(f"Copied key: {source_key} -> {dest_key} (read-only, owner only)")

        if source_ca and os.path.exists(source_ca):
            dest_ca = os.path.join(dest_dir, 'ca.crt')
            # Remove existing read-only file if present
            if os.path.exists(dest_ca):
                os.remove(dest_ca)
            shutil.copy2(source_ca, dest_ca)
            os.chmod(dest_ca, 0o444)  # Read-only for all
            logger.info(f"Copied CA: {source_ca} -> {dest_ca} (read-only)")

        return dest_cert, dest_key, dest_ca
    except Exception as e:
        logger.error(f"Error copying certificate files to {dest_dir}: {e}")
        return None, None, None

def update_certificates(component: str) -> bool:
    """Copy certificates from original paths to runtime directory.

    The loaded_* global variables point to constant runtime directory paths.
    This function copies the certificates from parsed_args (original paths) to those locations.

    Args:
        component: Either 'exporter' or 'management'

    Returns:
        True if successful, False otherwise.
    """
    if component == 'exporter':
        source_cert = parsed_args.exporter_cert
        source_key = parsed_args.exporter_key
        source_ca = parsed_args.exporter_ca
        dest_dir = EXPORTER_TLS_RUNTIME_DIR
    elif component == 'management':
        if not parsed_args.mgmt_use_tls:
            return True  # Nothing to do
        source_cert = parsed_args.mgmt_cert
        source_key = parsed_args.mgmt_key
        source_ca = parsed_args.mgmt_ca
        dest_dir = MANAGEMENT_TLS_RUNTIME_DIR
    else:
        logger.error(f"Invalid certificate component: {component}")
        return False

    dest_cert, dest_key, dest_ca = copy_cert_files(
        source_cert,
        source_key,
        source_ca,
        dest_dir
    )

    if not (dest_cert and dest_key):
        logger.error(f"Failed to copy {component} certificates to runtime directory")
        return False

    logger.info(f"{component.capitalize()} certificates copied to runtime directory")
    return True

def initialize_certificates():
    """Initialize certificates at startup by copying to runtime directory.

    Copies certificates from original paths (parsed_args) to constant runtime directory paths.
    If copying fails, the application will attempt to read from runtime paths which may not exist.
    """
    # Try to copy exporter certs to runtime dir
    if has_exporter_certs and not update_certificates('exporter'):
        raise Exception("Failed to copy exporter certificates to runtime directory")

    # Try to copy management certs to runtime dir
    if has_mgmt_certs and not update_certificates('management'):
        raise Exception("Failed to copy management certificates to runtime directory")

def start_prometheus_server():
    """Start prometheus HTTP server using prometheus_client built-in TLS support.

    Certificate rotation can be triggered via:
    - SIGHUP signal for manual reload
    - Auto-reload when file changes are detected (if enabled via SIGUSR2)
    """
    global prometheus_server, prometheus_thread
    port = int(parsed_args.port)
    is_https = loaded_exporter_cert and loaded_exporter_key
    https_kwargs = {
        'certfile': loaded_exporter_cert,
        'keyfile': loaded_exporter_key,
        'client_cafile': loaded_exporter_ca if loaded_exporter_ca else None,
        'client_auth_required': parsed_args.exporter_client_auth_required
    } if is_https else {}

    logger.info(f"Starting prometheus server on port {port}. IS_HTTPS: {is_https}")
    prometheus_server, prometheus_thread = start_http_server(port, **https_kwargs)
    _setup_signal_handlers()
    logger.info(f"Server started successfully")

    # Initialize certificate modification times for change detection (exporter + management)
    certs_list = (exporter_certs if has_exporter_certs else []) + (mgmt_certs if has_mgmt_certs else [])
    if certs_list:
        check_certificate_changes(certs_list)

def reload_management_certificates() -> bool:
    """Reload management certificates by copying to runtime dir and clearing connection cache."""
    if not parsed_args.mgmt_use_tls:
        logger.warning("Management certificate reload requested but TLS is disabled for management connections")
        return False

    logger.info("Reloading management certificates...")
    try:
        # Update certificates (copies from parsed_args to runtime dir, updates loaded_mgmt_*)
        if not update_certificates('management'):
            return False

        # Clear connection cache so new connections will use the updated loaded_mgmt_* certificates
        RestParser._connection_cache.clear()
        logger.info("Management connection cache cleared - connections will use new certificates")

        # Update metrics
        if cert_reload_counter:
            cert_reload_counter.labels(cert_component='management').inc()
        if cert_reload_timestamp:
            cert_reload_timestamp.labels(cert_component='management').set(time.time())

        return True
    except Exception as e:
        logger.error(f"Management certificate reload failed: {e}")
        return False

def reload_exporter_certificates() -> bool:
    """Reload exporter certificates by copying to runtime dir and restarting the Prometheus server."""
    global prometheus_server, prometheus_thread

    with cert_reload_lock:
        logger.info("Reloading exporter certificates (server restart)...")

        try:
            # Update certificates (copies from parsed_args to runtime dir, updates loaded_exporter_*)
            if not update_certificates('exporter'):
                return False

            port = int(parsed_args.port)

            # 1. Shutdown Prometheus server
            logger.info("Shutting down existing server...")
            try:
                prometheus_server.shutdown()
                prometheus_server.server_close()
                logger.info("Server shutdown completed successfully")
            except Exception as e:
                logger.warning(f"Server shutdown failed: {e}")

            # 2. Wait for server thread to finish
            if prometheus_thread.is_alive():
                logger.info("Waiting for server thread to finish...")
                prometheus_thread.join(timeout=10)
                if prometheus_thread.is_alive():
                    logger.warning("Server thread did not finish within timeout")

            # 3. Wait for port availability and restart server
            logger.info(f"Waiting for port {port} to become available...")
            try:
                wait_for_it(lambda: is_port_available(port), timeout=30).assert_result()
                logger.info("Port became available")
            except Exception:
                logger.warning(f"Port {port} remains unavailable after 30 seconds")

                # Check if we can use SO_REUSEADDR as fallback
                usage_info = check_port_usage(port)
                if "not found in netstat output" in usage_info:
                    logger.info("Port may be in TIME_WAIT state - will attempt SO_REUSEADDR fallback")
                else:
                    logger.error(f"Port conflicts detected: {usage_info}")
                    return False

            # 4. Restart server with new certificates
            logger.info("Restarting server with new certificates...")
            try:
                start_prometheus_server()
                logger.info("Exporter certificate reload completed successfully!")

                # Update metrics
                if cert_reload_counter:
                    cert_reload_counter.labels(cert_component='exporter').inc()
                if cert_reload_timestamp:
                    cert_reload_timestamp.labels(cert_component='exporter').set(time.time())

                return True
            except OSError as e:
                if "Address already in use" in str(e):
                    logger.warning(f"Port {port} still in use, trying SO_REUSEADDR fallback...")

                    # Fallback: Use SO_REUSEADDR to handle TIME_WAIT state
                    try:
                        original_socket = socket.socket

                        def reuse_socket(*args, **kwargs):
                            sock = original_socket(*args, **kwargs)
                            if sock.family == socket.AF_INET and sock.type == socket.SOCK_STREAM:
                                sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                            return sock

                        socket.socket = reuse_socket
                        try:
                            start_prometheus_server()
                            logger.info("Certificate reload successful with SO_REUSEADDR")
                            return True
                        finally:
                            socket.socket = original_socket

                    except Exception as fallback_e:
                        logger.error(f"Certificate reload failed: {fallback_e}")
                        return False
                else:
                    logger.error(f"Server startup failed: {e}")
                    return False

        except Exception as e:
            logger.error(f"Certificate reload failed: {e}")
            return False

def _setup_signal_handlers():
    """Set up signal handlers for certificate reload, auto-reload toggle, and memory profiler."""

    def handle_sighup(signum, frame):
        """Reload both exporter and management certificates (manual trigger)"""
        logger.info("Received SIGHUP signal - reloading both exporter and management certificates")
        try:
            # Reload exporter certificates
            if has_exporter_certs:
                logger.info("Reloading exporter certificates...")
                if reload_exporter_certificates():
                    logger.info("Exporter certificates reloaded successfully")
                else:
                    logger.error("Failed to reload exporter certificates")

            # Reload management certificates
            if has_mgmt_certs:
                logger.info("Reloading management certificates...")
                if reload_management_certificates():
                    logger.info("Management certificates reloaded successfully")
                else:
                    logger.error("Failed to reload management certificates")
        except Exception as e:
            logger.error(f"Error during certificate reload: {e}")

    def handle_sigusr1(signum, frame):
        """Toggle memory profiler on/off"""
        global profiler_enabled, profiler
        profiler_enabled = not profiler_enabled

        if profiler_enabled:
            logger.info("Received SIGUSR1 signal - ENABLING memory profiler")
            # Profiler will be initialized on next cycle if needed
        else:
            logger.info("Received SIGUSR1 signal - DISABLING memory profiler")
            # Profiler stays in memory but stops reporting

    def handle_sigusr2(signum, frame):
        """Toggle certificate auto-reload on/off"""
        global cert_auto_reload
        cert_auto_reload = not cert_auto_reload

        if cert_auto_reload:
            logger.info("Received SIGUSR2 signal - ENABLING certificate auto-reload")
            logger.info("Certificates will now be automatically reloaded when file changes are detected")
        else:
            logger.info("Received SIGUSR2 signal - DISABLING certificate auto-reload")
            logger.info("Certificates will only be reloaded via SIGHUP signal")

    try:
        signal.signal(signal.SIGHUP, handle_sighup)
        signal.signal(signal.SIGUSR1, handle_sigusr1)
        signal.signal(signal.SIGUSR2, handle_sigusr2)
        logger.info("Signal handlers configured:")
        logger.info("  SIGHUP  = Reload both exporter and management certificates (manual)")
        logger.info("  SIGUSR1 = Toggle memory profiler")
        logger.info(f"  SIGUSR2 = Toggle certificate auto-reload (currently: {'enabled' if cert_auto_reload else 'disabled'})")
    except Exception as e:
        logger.warning(f"Failed to register signal handlers: {e}")


def get_query_label_values(obj: Any, query_labels: Dict[str, str]) -> Dict[str, Union[int, float, bool, str]]:
    """ Take raw dictionary, query labels and return values dict """
    label_values = {}
    for name, query in query_labels.items():
        value = jmespath.search(query, obj)
        assert isinstance(value, str) or not isinstance(value, Iterable), f"Bad query '{query}' returned a non-string iterable value: '{str(value)[:50]}...'"
        label_values[name] = value

    return label_values


def get_query_labels_list_values(obj: Any, query_labels_list: Dict[str, str]) -> Dict[str, Union[int, float, bool, str]]:
    """ Take raw dictionary, query labels list and return values dict """
    return {k: v for k, v in dict(item.split("=", 1) for item in jmespath.search(query_labels_list['path'], obj).split(";")).items() if k in query_labels_list['names']}


def load_metrics_config() -> Dict[str, list]:
    """ Load metrics configuration from yaml file into a map """
    # TODO: return dict with parser types vs lists, it can be passed over to populate_metrics
    sources = defaultdict(list)
    try:
        with open(parsed_args.conf, 'r') as fp:
            m_yaml = yaml.safe_load(fp.read())
            versioned_metrics_conf = defaultdict(defaultdict)
            for source, metrics_conf in m_yaml.items():
                extends = metrics_conf.pop('extends', None)
                if extends:
                    metrics_conf = merge_dicts(deepcopy(versioned_metrics_conf[source.split('|')[0]][extends]), metrics_conf)

                for dpath in metrics_conf.pop('drops', []):
                    del_path(metrics_conf, dpath)

                source_name, sep, source_version = source.partition(MetricParser._version_delimiter)
                if not sep:
                    source_version = MetricParser._default_version
                versioned_metrics_conf[source_name][source_version] = metrics_conf

            for source, parsing_strategies in versioned_metrics_conf.items():
                # TODO: need to create a better way for 'cross-version' configuration than using parsing_strategies['0']
                parser_class = getattr(sys.modules[__name__], parsing_strategies['0'].get('type', 'JsonProcParser'))
                if issubclass(parser_class, RestParser) and parsing_strategies['0'].get('per_node', False):
                    for m in mgmt_hosts:
                        SOURCE_CONF[f'{m}@{source}'] = parser_class(source, parsing_strategies, endpoint=m)
                else:
                    SOURCE_CONF[source] = parser_class(source, parsing_strategies)

                if issubclass(parser_class, ProcParser):
                    sources['proc'].append(source)
                elif issubclass(parser_class, RestParser):
                    if parsing_strategies['0'].get('per_node', False):
                        for m in mgmt_hosts:
                            sources['rest'].append(f'{m}@{source}')
                    else:
                        sources['rest'].append(source)
                elif issubclass(parser_class, UmRpcParser):
                    sources['rpc'].append(source)
                elif issubclass(parser_class, CmdParser):
                    sources['cmd'].append(source)

    except Exception as e:
        logger.error(f"Unable to open metrics configuration file {parsed_args.conf}. {repr(e)}")
        raise e

    return sources


def metrify(obj: Any, metrics: Dict[Union[re.Pattern, List], MetricWrapperBase], name: str, proc_pattern: str, extra_labels: dict):
    """ Flatten a dictionary, extract the metrics and set value in prometheus client """
    if isinstance(obj, dict):
        for key in obj:
            metric_name = (f'{name}_{key}' if name else key).lower()
            metric_name = reduce(lambda k, v: k.replace(*v), {'>': 'more_than_', ' ': '_'}.items(), metric_name)
            metrify(obj[key], metrics, metric_name, proc_pattern, extra_labels)
        return

    if isinstance(obj, (int, float)) or isinstance(obj, str) and obj.isnumeric():
        labels = extra_labels.copy()
        if name in metrics:
            metric = metrics[name]
        else:
            for expr, metric_data in metrics.items():
                metric = metric_data
                try:
                    labels.update(expr.search(name).groupdict())
                    break
                except (TypeError, AttributeError):
                    pass
            else:
                # Couldn't find metric definition
                return

        logger.debug(f"metric: {name}, extra_labels: {extra_labels}, labels: {labels}, labelnames: {metric._labelnames}, value: {obj}")

        try:
            MetricParser.add_active_metric(metric, tuple(labels[l] for l in metric._labelnames))
            metric.labels(**labels).set(obj)
        except Exception as e:
            logger.error(f'Failed setting {name}{labels}={obj}. Error: {repr(e)}')


def aggregate(obj: Any, aggregates: dict, extra_labels: dict):
    """ Init metrics for all label permutation, increment queried metrics """
    for name, agg in aggregates.items():
        metric_specific_labels = {l: v for l, v in extra_labels.items() if l not in agg.ignore}
        if agg.query_labels:
            metric_specific_labels.update(get_query_label_values(obj, agg.query_labels))
        if agg.query_labels_list:
            metric_specific_labels.update(get_query_labels_list_values(obj, agg.query_labels_list))

        results = []
        for agg_conf in agg.aggregate_by.values():
            query = agg_conf['query']
            res = jmespath.search(query, obj)
            assert not isinstance(res, Mapping), f"Bad query '{query}' returned a Mapping value: '{str(res)[:50]}...'"
            if isinstance(res, str) or not isinstance(res, Iterable):
                res = [res]

            results.append(res)

        multi_agg_labels2values = defaultdict(list)
        for _res in zip(*results):
            agg_labels = {l: v for l, v in zip(agg.aggregate_by.keys(), _res) if l not in agg.ignore}
            agg_labels.update(metric_specific_labels)
            hashable_sorted_labels = tuple(sorted(agg_labels.items()))
            multi_agg_labels2values[hashable_sorted_labels].append(jmespath.search(agg.sum_by, obj) if agg.sum_by else 1)

        # Increment queried metrics
        for agg_labels, values in multi_agg_labels2values.items():
            with AGG_LOCK:
                try:
                    AGG_CNTR[name][agg_labels] += sum(values)
                except KeyError as e:
                    logger.debug(f"Dumping AGG_CNTR[{name}] = {AGG_CNTR[name]}. agg_label value: {AGG_CNTR[name].get(agg_labels)}")


def iterate_collection(collection: Any, parsing_strategy: ParsingStrategy, path: str, pattern: str, extra_labels: dict):
    for obj in collection:
        e_lables = extra_labels.copy()
        try:
            # calculate general query labels
            if getattr(parsing_strategy, 'query_labels'):
                e_lables.update(get_query_label_values(obj, parsing_strategy.query_labels))
            if getattr(parsing_strategy, 'query_labels_list'):
                e_lables.update(get_query_labels_list_values(obj, parsing_strategy.query_labels_list))
        except (KeyError, AttributeError) as e:
            logger.debug(f"Unable to calculate general query label of file {path} - {repr(e)}")

        if obj is None or (isinstance(obj, Iterable) and not obj):
            # [], {}, or None - no reason to work on
            continue

        for c_name in parsing_strategy.collections:
            iterate_collection(jmespath.search(c_name, obj) or [], parsing_strategy.collections[c_name], path, pattern, e_lables)

        if parsing_strategy.metrics:
            metrify(obj, parsing_strategy.metrics, parsing_strategy.metric_prefix, pattern, e_lables)

        if parsing_strategy.aggregates:
            aggregate(obj, parsing_strategy.aggregates, e_lables)


def _populate_metrics(path: str, pattern: str):
    """ Gather all needed info about the source and call metrify """
    logger.debug(f"SOURCE: {path}, PATTERN: {pattern}")
    try:
        metric_parser = SOURCE_CONF[pattern]
        raw_dict, version = metric_parser.get_raw_data(path)
        versioned_metric_parser = metric_parser.version2metric[version]
        transform = versioned_metric_parser.transform
        if transform:
            raw_dict = jmespath.search(transform, raw_dict)

        extra_labels = {**metric_parser.parser_labels(), **versioned_metric_parser.additional_labels()}
        try:
            # calculate glob labels
            if getattr(versioned_metric_parser, 'glob_re'):
                extra_labels.update(versioned_metric_parser.glob_re.search(path).groupdict())
        except (KeyError, AttributeError) as e:
            logger.debug(f"Unable to calculate glob label of file {path} - {repr(e)}")

        collection = raw_dict if raw_dict and isinstance(raw_dict, list) else [raw_dict]
        iterate_collection(collection, versioned_metric_parser, path, pattern, extra_labels)
    except (ConnectionError, ConnectionManagerError) as e:
        logger.debug(f'Not populating metric - {repr(e)}')
    except Exception as e:
        logger.info(f'Unable to work on source: {path} - {repr(e)}')


def populate_metrics(sources: Dict[str, list]):
    """ Multi-threaded call to _populate_metrics function """
    # TODO: Don't open and close a thread everytime - manage consistent threads per fpath and keep scanning for new files
    global AGG_CNTR
    AGG_CNTR = defaultdict(lambda: defaultdict(int))

    path2source = [(fpath, source) for source in sources['proc'] for fpath in glob.glob(source)]
    path2source += [(source.partition('@')[-1], source) if '@' in source else (source, source) for source in sources['rest']]

    if UmRpcParser.is_rpc_available:
        for source in sources['rpc']:
            um_rpc_parser_class : Type[UmRpcParser] = SOURCE_CONF[source].__class__
            if hasattr(um_rpc_parser_class, 'get_iterables'):
                path2source += [(um_rpc_parser_class.build_rpc_cmd(source, elem), source) for elem in um_rpc_parser_class.get_iterables(source)]
            else:
                path2source += [(source, source)]

    with ThreadPoolExecutor(max_workers=MAX_CONCURRENT_FOPEN) as executor:
        executor.map(lambda source_args: _populate_metrics(*source_args), path2source)

    for agg_name, labels2counter in AGG_CNTR.items():
        for labels, count in labels2counter.items():
            try:
                metric = REGISTRY._names_to_collectors[agg_name]
                ldict = dict(labels)
                MetricParser.add_active_metric(metric, tuple(ldict[l] for l in metric._labelnames))
                REGISTRY._names_to_collectors[agg_name].labels(**ldict).set(count)
            except KeyError as e:
                # nothing to populate here yet
                pass


class ReloadExporterServerError(Exception):
    pass


@instrumented_loop(
    enable_timing=True,
    enable_gc=True,
    gc_interval=60
)
def metrics_collection_loop(sources: Dict[str, list]):
    """
    Main metrics collection loop.

    Core responsibilities:
    - Decrease metric thresholds for stale metrics
    - Populate metrics from all configured sources
    - Remove inactive metrics

    Note: Timing, profiling, optional certificate auto-reload, garbage collection,
    and other instrumentation is handled by the @instrumented_loop decorator.
    Certificate auto-reload is disabled by default and can be enabled via SIGUSR2.
    """
    MetricParser.decrease_metric_threshold()
    populate_metrics(sources)
    MetricParser.remove_inactive_metrics()


def main():
    logger.info(f'Starting NVMesh prometheus metrics exporter {EXPORTER_VERSION} on {host.name}: {parsed_args}')
    sources = load_metrics_config()
    logger.debug(f"metrics config loaded: {SOURCE_CONF}")

    initialize_certificates()
    start_prometheus_server()

    try:
        while not time.sleep(parsed_args.interval):
            metrics_collection_loop(sources)
    except KeyboardInterrupt:
        logger.info("Metrics collection interrupted by user")
    except Exception as e:
        logger.error(f"Metrics collection error: {e}")
        raise


if __name__ == '__main__':
    main()
