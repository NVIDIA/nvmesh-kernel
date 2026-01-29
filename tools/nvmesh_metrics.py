# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import json
import math
from abc import ABC, abstractmethod
from typing import Dict, List, Set, Type, Optional, Union


class MetricFactory:
    _registry: Dict[str, Type['MetricBase']] = {}

    @classmethod
    def register(cls, name: str):
        def decorator(metric_cls: Type['MetricBase']):
            cls._registry[name] = metric_cls
            return metric_cls
        return decorator

    @classmethod
    def from_json(cls, data: Dict) -> 'MetricBase':
        metric_type = data.get("type")
        if metric_type not in cls._registry:
            raise ValueError(f"Unknown metric type: {metric_type}")
        return cls._registry[metric_type].from_json(data)


class MetricBase(ABC):
    def __init__(self, name: str, labels: str):
        self.name = name
        self.labels: Set[str] = set(labels.split(';')) if labels else set()

    @classmethod
    @abstractmethod
    def from_json(cls, data: Dict) -> 'MetricBase':
        ...

    @abstractmethod
    def update(self, value: int):
        ...

    @abstractmethod
    def merge(self, other: 'MetricBase'):
        ...

    @abstractmethod
    def clear(self):
        ...

    @abstractmethod
    def to_json(self) -> Dict:
        ...

    @staticmethod
    def read_memmgr_info(source: Union[str, Dict]) -> List['MetricBase']:
        """
        Read memory manager info from a JSON file or directly from a JSON dict.
        `source` can be a filepath or a dict.
        Returns a list of MetricBase instances.
        """
        if isinstance(source, str):
            with open(source, 'r') as f:
                data = json.load(f)
        elif isinstance(source, dict):
            data = source
        else:
            raise ValueError("source must be a filepath or a dict")

        if any(key.startswith("metrics.cpu") for key in data):
            return fold_metrics(data)
        else:
            return [MetricFactory.from_json(metric) for metric in data.get('metrics.all_cpus', [])]


def fold_metrics(data: Dict) -> List[MetricBase]:
    folded = {}

    for key, metrics in data.items():
        if not key.startswith("metrics.cpu"):
            continue

        cpu_index = int(key[len("metrics.cpu"):])

        for metric_dict in metrics:
            metric_type = metric_dict["type"]
            name = metric_dict["name"]
            labels = metric_dict["labels"]
            comp = next((l.split("=")[1] for l in labels.split(";") if l.startswith("component=")), None)
            if not comp:
                continue

            key_id = (comp, name, labels)

            if key_id not in folded:
                if metric_type == "gauge":
                    folded[key_id] = Gauge(name, labels, 0)
                elif metric_type == "max_val":
                    folded[key_id] = MaxValue(name, labels, 0)
                elif metric_type == "monotonic_counter":
                    folded[key_id] = MonotonicCounter(name, labels, 0)
                elif metric_type == "bytes_histogram":
                    folded[key_id] = BytesHistogram(name, labels, [0] * len(metric_dict["values"]))
                else:
                    raise ValueError(f"Unknown metric type: {metric_type}")

            folded[key_id].merge(MetricFactory.from_json(metric_dict))

    return list(folded.values())


class HistogramBase(MetricBase):
    BINS: int  # Must be set by subclass
    SHIFT: int = 0  # Optional, default 0

    def __init__(self, name: str, labels: str, bins: Optional[List[int]] = None):
        super().__init__(name, labels)
        if not hasattr(self, 'BINS'):
            raise NotImplementedError("Subclass must define BINS")
        if bins is None:
            self.bins = [0] * self.BINS
        else:
            self.bins = bins

    def clear(self):
        self.bins = [0] * self.BINS

    @property    
    def total_volume(self) -> int:
        total = 0
        for idx in range(self.BINS):
            total += self.bins[idx] * (1 << (idx + self.SHIFT))
        return total

    def merge(self, other: 'HistogramBase'):
        if not isinstance(other, HistogramBase):
            raise TypeError("Can only merge with another HistogramBase")
        if len(self.bins) != len(other.bins):
            raise ValueError("Bin lengths do not match")
        for i in range(self.BINS):
            self.bins[i] += other.bins[i]

    def subtract(self, other: 'HistogramBase') -> 'HistogramBase':
        if not isinstance(other, HistogramBase):
            raise TypeError("Can only subtract another HistogramBase")
        if len(self.bins) != len(other.bins):
            raise ValueError("Bin lengths do not match")
        diff_bins = [a - b for a, b in zip(self.bins, other.bins)]
        return self.__class__(self.name, ";".join(self.labels), diff_bins)

    @staticmethod
    def _get_bin_index(value: int, shift: int, n_bins: int) -> int:
        v = value >> shift
        idx = 0
        while v > 1 and idx < n_bins - 1:
            v >>= 1
            idx += 1
        return idx


@MetricFactory.register("bytes_histogram")
class BytesHistogram(HistogramBase):
    BINS = 18
    SHIFT = 3

    def update(self, value: int):
        if value == 0:
            return
        idx = self._get_bin_index(value, self.SHIFT, self.BINS)
        self.bins[idx] += 1

    @classmethod
    def from_json(cls, data: Dict) -> 'BytesHistogram':
        return cls(data["name"], data["labels"], data["values"])

    def to_json(self) -> Dict:
        return {
            "type": "bytes_histogram",
            "name": self.name,
            "labels": ";".join(self.labels),
            "values": self.bins
        }


@MetricFactory.register("gauge")
class Gauge(MetricBase):
    def __init__(self, name: str, labels: str, counter: int):
        super().__init__(name, labels)
        self.counter = counter

    def update(self, value: int):
        self.counter += value

    def merge(self, other: 'Gauge'):
        self.counter += other.counter

    def clear(self):
        raise RuntimeError("Clear not allowed for Gauge")

    @classmethod
    def from_json(cls, data: Dict) -> 'Gauge':
        return cls(data["name"], data["labels"], data["value"])

    def to_json(self) -> Dict:
        return {
            "type": "gauge",
            "name": self.name,
            "labels": ";".join(self.labels),
            "value": self.counter
        }


@MetricFactory.register("max_val")
class MaxValue(MetricBase):
    def __init__(self, name: str, labels: str, value: int):
        super().__init__(name, labels)
        self.max_value = value

    def update(self, value: int):
        if value > self.max_value:
            self.max_value = value

    def merge(self, other: 'MaxValue'):
        if other.max_value > self.max_value:
            self.max_value = other.max_value

    def clear(self):
        self.max_value = 0

    @classmethod
    def from_json(cls, data: Dict) -> 'MaxValue':
        return cls(data["name"], data["labels"], data["value"])

    def to_json(self) -> Dict:
        return {
            "type": "max_val",
            "name": self.name,
            "labels": ";".join(self.labels),
            "value": self.max_value
        }


@MetricFactory.register("monotonic_counter")
class MonotonicCounter(MetricBase):
    def __init__(self, name: str, labels: str, counter: int):
        super().__init__(name, labels)
        self.counter = counter

    def update(self, value: int):
        self.counter += value

    def merge(self, other: 'MonotonicCounter'):
        self.counter += other.counter

    def clear(self):
        self.counter = 0

    @classmethod
    def from_json(cls, data: Dict) -> 'MonotonicCounter':
        return cls(data["name"], data["labels"], data["value"])

    def to_json(self) -> Dict:
        return {
            "type": "monotonic_counter",
            "name": self.name,
            "labels": ";".join(self.labels),
            "value": self.counter
        }

