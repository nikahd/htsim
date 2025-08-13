import ast
from functools import reduce
import itertools
import operator
import os
from typing import Any, Callable, Iterable, List

FILE_PATH = os.path.realpath(__file__)  # PROJECT_ROOT/scripts/common.py
SCRIPTS_PATH = os.path.dirname(FILE_PATH)  # PROJECT_ROOT/scripts
PROJECT_ROOT = os.path.dirname(SCRIPTS_PATH)

HTSIM_DATACENTER_ROOT = os.path.join(PROJECT_ROOT, "htsim", "sim", "datacenter")
TRAFFIC_GEN_ROOT = os.path.join(PROJECT_ROOT, "traffic_gen")
TRAFFIC_GEN_CDF_FILES_ROOT = os.path.join(TRAFFIC_GEN_ROOT, "cdf_files")

# Conversions
S_TO_US = 1e6
S_TO_MS = 1e3
MS_TO_US = 1e3
US_TO_MS = 1e-3
US_TO_S = 1e-6
MS_TO_S = 1e-3
NS_TO_MS = 1e-6
NS_TO_US = 1e-3
NS_TO_PS = 1e3
PS_TO_US = 1e-6
US_TO_NS = 1e3

BYTES_TO_BITS = 8

Gb_TO_b = 1e9
Gb_TO_Mb = 1e3
Mb_TO_b = 1e6
Kb_TO_b = 1e3

GiB_TO_B = 1 << 30
MiB_TO_B = 1 << 20
KiB_TO_B = 1 << 10

B_TO_KiB = 1.0 / KiB_TO_B


def parse_literal(element: str):
    """Converts string to literal if possible, else returns the string

    Examples
    --------
    >>> parse_literal("1.0")
    1.0
    >>> parse_literal("1")
    1
    >>> type(parse_literal("1"))
    <class 'int'>
    >>> type(parse_literal("1.0"))
    <class 'float'>
    """

    try:
        return ast.literal_eval(element)
    except ValueError:
        return element


def parse_info(fpath: str, ext: str):
    """For storing experiment log files, configuration files, and inputs, 
    we follow a convention of storing the important parameter values (e.g.,
    nodes in topology, cc algorithm being used) in the file name as key/value
    pairs.

    This function, parses parameter key, value pairs from the filename. This
    function assumes the convention that key, value pairs are separated by ':'.
    Further, the keys and values are separated by '='.

    An example filename is thus:
    <key1>=<val1>:<key2>=<val2>:...:<keyN>=<valN>.<ext>

    Examples
    --------
    >>> parse_info("over_sub=1:node=128.json", ".json")
    {'over_sub': 1, 'node': 128}
    """

    fname = os.path.basename(fpath).removesuffix(ext)
    ret = {}
    for param_tag in fname.split(':'):
        param_name = param_tag.split('=')[0]
        param_val = param_tag.split('=')[1]
        ret[param_name] = parse_literal(param_val)
    return ret


def get_info_string(info):
    """Convert dict of parameter key/value pairs to string representation.
    Useful for populating figure titles/dashboards.

    Examples
    --------
    >>> get_info_string({'over_sub': 1, 'node': 128})
    'over_sub=1, node=128'
    """
    return ', '.join([f'{k}={v}' for k, v in info.items()])


def try_except(function: Callable):
    """This function is useful for debugging in python. If we call a function
    through `try_except` or decorate a function with the `try_except_wrapper`,
    then we get a python interactive debugger anytime an exception is raised in
    the function.

    Examples
    --------
    >>> def f():
    ...     raise ValueError
    >>> try_except(f)
    Traceback (most recent call last):
    ...
    ipdb>

    >>> @try_except_wrapper
    ... def f():
    ...     raise ValueError
    >>> f()
    Traceback (most recent call last):
    ...
    ipdb>
    """
    try:
        return function()
    except Exception:
        import sys
        import traceback

        import ipdb
        extype, value, tb = sys.exc_info()
        traceback.print_exc()
        ipdb.post_mortem(tb)


def try_except_wrapper(function):
    """See documentation of `try_except`"""

    def func_to_return(*args, **kwargs):
        def func_to_try():
            return function(*args, **kwargs)
        return try_except(func_to_try)
    return func_to_return


class PartialParams(dict):
    """For experiments, we want to run different combination of parameter
    configurations. For instance, we may want to run an experiments such that
    when topology size is 128 nodes, we want to explore over subscription ratios
    of 1 and 8, whereas when topology size is 1024 nodes, we want to explore
    over subscription ratios of 4 and 16.

    This class helps manage and manipulate such configuration/inputs for
    experiments to maintain a list of all possible configurations we want to run
    for an experiment. It helps take cartesian product and union of parameter
    lists. The code example below shows how to setup configurations for the
    above use case.

    Examples
    --------
    >>> l1 = [PartialParams(over_sub=1), PartialParams(over_sub=8)]
    >>> l2 = [PartialParams(nodes=128)]
    >>> pdt1 = PartialParams.product(l1, l2)
    >>> pdt1
    [PartialParams(over_sub=1, nodes=128), PartialParams(over_sub=8, nodes=128)]
    >>> l1 = [PartialParams(over_sub=4), PartialParams(over_sub=16)]
    >>> l2 = [PartialParams(nodes=1024)]
    >>> pdt2 = PartialParams.product(l1, l2)
    >>> pdt2
    [PartialParams(over_sub=4, nodes=1024), PartialParams(over_sub=16, nodes=1024)]
    >>> PartialParams.consolidate(pdt1, pdt2)
    [PartialParams(over_sub=1, nodes=128), PartialParams(over_sub=8, nodes=128), PartialParams(over_sub=4, nodes=1024), PartialParams(over_sub=16, nodes=1024)]

    Another use case could be managing configuration when different algorithms
    uses different number of parameters and we want to explore different
    parameter combinations within an algorithm. For instance, say we want to run
    experiment comparing dctcp cc algorithm with ecmp vs. smartt cc algorithm
    with spraying and reps. Below code shows how to generate configurations for
    this set of experiments.

    >>> dctcp = [PartialParams(cc_algo='dctcp', load_balancing_algo="ecmp")]
    >>> smartt = [PartialParams(cc_algo='smartt')]
    >>> smartt_lb_choices = [PartialParams(load_balancing_algo="spraying"), PartialParams(load_balancing_algo="reps")]
    >>> smartt_exps = PartialParams.product(smartt, smartt_lb_choices)
    >>> smartt_exps
    [PartialParams(cc_algo='smartt', load_balancing_algo='spraying'), PartialParams(cc_algo='smartt', load_balancing_algo='reps')]
    >>> all_exps = PartialParams.consolidate(dctcp, smartt_exps)
    >>> all_exps
    [PartialParams(cc_algo='dctcp', load_balancing_algo='ecmp'), PartialParams(cc_algo='smartt', load_balancing_algo='spraying'), PartialParams(cc_algo='smartt', load_balancing_algo='reps')]

    For this simple example, we can obviously directly write the final list of
    configurations. But the product and union functions help as the number of
    parameter chocies/combinations we want to explore grows.
    """

    @staticmethod
    def merge(*l: Iterable):
        return PartialParams(**reduce(operator.ior, l, {}))

    @staticmethod
    def product(*l: List):
        return [PartialParams(PartialParams.merge(*x)) for x in itertools.product(*l)]

    @staticmethod
    def consolidate(*l: Iterable):
        ret = []
        for x in l:
            if isinstance(x, PartialParams):
                ret.append(x)
            else:
                assert isinstance(x, Iterable)
                ret.extend(PartialParams.consolidate(*x))
        return ret
