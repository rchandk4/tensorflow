#!/usr/bin/env python
# Copyright 2015 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================

"""Crosstool wrapper for compiling CUDA programs.

SYNOPSIS:
  crosstool_wrapper_is_not_gcc [options passed in by cc_library()
                                or cc_binary() rule]

DESCRIPTION:
  This script is expected to be called by the cc_library() or cc_binary() bazel
  rules. When the option "-x cuda" is present in the list of arguments passed
  to this script, it invokes the nvcc CUDA compiler. Most arguments are passed
  as is as a string to --compiler-options of nvcc. When "-x cuda" is not
  present, this wrapper invokes hybrid_driver_is_not_gcc with the input
  arguments as is.

NOTES:
  Changes to the contents of this file must be propagated from
  //third_party/gpus/crosstool/crosstool_wrapper_is_not_gcc to
  //third_party/gpus/crosstool/v*/*/clang/bin/crosstool_wrapper_is_not_gcc
"""

from __future__ import print_function

__author__ = 'keveman@google.com (Manjunath Kudlur)'

from argparse import ArgumentParser
import os
import subprocess
import re
import sys
import pipes

# Template values set by cuda_autoconf.
CPU_COMPILER = ('%{cpu_compiler}')
GCC_HOST_COMPILER_PATH = ('%{gcc_host_compiler_path}')

CURRENT_DIR = os.path.dirname(sys.argv[0])
NVCC_PATH = CURRENT_DIR + '/../../../cuda/bin/nvcc'
LLVM_HOST_COMPILER_PATH = ('/usr/bin/gcc')
PREFIX_DIR = os.path.dirname(GCC_HOST_COMPILER_PATH)

def Log(s):
  print('gpus/crosstool: {0}'.format(s))


def GetOptionValue(argv, option):
  """Extract the list of values for option from the argv list.

  Args:
    argv: A list of strings, possibly the argv passed to main().
    option: The option whose value to extract, without the leading '-'.

  Returns:
    A list of values, either directly following the option,
    (eg., -opt val1 val2) or values collected from multiple occurrences of
    the option (eg., -opt val1 -opt val2).
  """

  parser = ArgumentParser()
  parser.add_argument('-' + option, nargs='*', action='append')
  args, _ = parser.parse_known_args(argv)
  if not args or not vars(args)[option]:
    return []
  else:
    return sum(vars(args)[option], [])


def GetHostCompilerOptions(argv):
  """Collect the -isystem, -iquote, and --sysroot option values from argv.

  Args:
    argv: A list of strings, possibly the argv passed to main().

  Returns:
    The string that can be used as the --compiler-options to nvcc.
  """

  parser = ArgumentParser()
  parser.add_argument('-isystem', nargs='*', action='append')
  parser.add_argument('-iquote', nargs='*', action='append')
  parser.add_argument('--sysroot', nargs=1)
  parser.add_argument('-g', nargs='*', action='append')

  args, _ = parser.parse_known_args(argv)

  opts = ''

  if args.isystem:
    opts += ' -isystem ' + ' -isystem '.join(sum(args.isystem, []))
  if args.iquote:
    opts += ' -iquote ' + ' -iquote '.join(sum(args.iquote, []))
  if args.g:
    opts += ' -g' + ' -g'.join(sum(args.g, []))
  if args.sysroot:
    opts += ' --sysroot ' + args.sysroot[0]

  return opts

def GetNvccOptions(argv):
  """Collect the -nvcc_options values from argv.

  Args:
    argv: A list of strings, possibly the argv passed to main().

  Returns:
    The string that can be passed directly to nvcc.
  """

  parser = ArgumentParser()
  parser.add_argument('-nvcc_options', nargs='*', action='append')

  args, _ = parser.parse_known_args(argv)

  if args.nvcc_options:
    return ' '.join(['--'+a for a in sum(args.nvcc_options, [])])
  return ''


def StripAndTransformNvccOptions(argv):
  """Strips the -nvcc_options values from argv and transforms define-macros.

  Args:
    argv: A list of strings, possibly the argv passed to main().

  Returns:
    A list of strings that can be passed directly to gcudacc.
  """
  parser = ArgumentParser()
  parser.add_argument('-nvcc_options', nargs='*', action='store')
  args, leftover = parser.parse_known_args(argv)
  if args.nvcc_options:
    for option in args.nvcc_options:
      (flag, _, value) = option.partition('=')
      if 'define-macro' in flag:
        leftover.append('-D' + value)
  return leftover


def InvokeGcudacc(argv, gcudacc_version, gcudacc_flags, log=False):
  """Call gcudacc with arguments assembled from argv.

  Args:
    argv: A list of strings, possibly the argv passed to main().
    gcudacc_version: The version of gcudacc; this is a subdirectory name under
      the gcudacc bin/ directory.
    gcudacc_flags: A list of extra arguments passed just for gcudacc.
    log: True if logging is requested.

  Returns:
    The return value of calling os.system('gcudacc ' + args)
  """

  gcudacc_cmd = os.path.join(GCUDACC_PATH_BASE, gcudacc_version, 'gcudacc.par')
  gcudacc_cmd = (
      gcudacc_cmd +
      ' --google_host_compiler={0} '.format(LLVM_HOST_COMPILER_PATH) +
      ' '.join(sum(gcudacc_flags, [])) +
      ' -- ' +
      ' '.join(StripAndTransformNvccOptions(argv)))
  if log: Log(gcudacc_cmd)
  return os.system(gcudacc_cmd)


def InvokeNvcc(argv, log=False):
  """Call nvcc with arguments assembled from argv.

  Args:
    argv: A list of strings, possibly the argv passed to main().
    log: True if logging is requested.

  Returns:
    The return value of calling os.system('nvcc ' + args)
  """

  host_compiler_options = GetHostCompilerOptions(argv)
  nvcc_compiler_options = GetNvccOptions(argv)
  opt_option = GetOptionValue(argv, 'O')
  m_options = GetOptionValue(argv, 'm')
  m_options = ''.join([' -m' + m for m in m_options if m in ['32', '64']])
  include_options = GetOptionValue(argv, 'I')
  out_file = GetOptionValue(argv, 'o')
  depfiles = GetOptionValue(argv, 'MF')
  defines = GetOptionValue(argv, 'D')
  defines = ''.join([' -D' + define for define in defines])
  undefines = GetOptionValue(argv, 'U')
  undefines = ''.join([' -U' + define for define in undefines])
  std_options = GetOptionValue(argv, 'std')
  # currently only c++11 is supported by Cuda 7.0 std argument
  nvcc_allowed_std_options = ["c++11"]
  std_options = ''.join([' -std=' + define
      for define in std_options if define in nvcc_allowed_std_options])

  # The list of source files get passed after the -c option. I don't know of
  # any other reliable way to just get the list of source files to be compiled.
  src_files = GetOptionValue(argv, 'c')

  if len(src_files) == 0:
    return 1
  if len(out_file) != 1:
    return 1

  opt = (' -O2' if (len(opt_option) > 0 and int(opt_option[0]) > 0)
         else ' -g -G')

  includes = (' -I ' + ' -I '.join(include_options)
              if len(include_options) > 0
              else '')

  # Unfortunately, there are other options that have -c prefix too.
  # So allowing only those look like C/C++ files.
  src_files = [f for f in src_files if
               re.search('\.cpp$|\.cc$|\.c$|\.cxx$|\.C$', f)]
  srcs = ' '.join(src_files)
  out = ' -o ' + out_file[0]

  supported_cuda_compute_capabilities = [ %{cuda_compute_capabilities} ]
  nvccopts = '-D_FORCE_INLINES '
  for capability in supported_cuda_compute_capabilities:
    capability = capability.replace('.', '')
    nvccopts += r'-gencode=arch=compute_%s,\"code=sm_%s,compute_%s\" ' % (
        capability, capability, capability)
  nvccopts += ' ' + nvcc_compiler_options
  nvccopts += undefines
  nvccopts += defines
  nvccopts += std_options
  nvccopts += m_options

  if depfiles:
    # Generate the dependency file
    depfile = depfiles[0]
    cmd = (NVCC_PATH + ' ' + nvccopts +
           ' --compiler-options "' + host_compiler_options + '"' +
           ' --compiler-bindir=' + GCC_HOST_COMPILER_PATH +
           ' -I .' +
           ' -x cu ' + includes + ' ' + srcs + ' -M -o ' + depfile)
    if log: Log(cmd)
    exit_status = os.system(cmd)
    if exit_status != 0:
      return exit_status

  cmd = (NVCC_PATH + ' ' + nvccopts +
         ' --compiler-options "' + host_compiler_options + ' -fPIC"' +
         ' --compiler-bindir=' + GCC_HOST_COMPILER_PATH +
         ' -I .' +
         ' -x cu ' + opt + includes + ' -c ' + srcs + out)

  # TODO(zhengxq): for some reason, 'gcc' needs this help to find 'as'.
  # Need to investigate and fix.
  cmd = 'PATH=' + PREFIX_DIR + ' ' + cmd
  if log: Log(cmd)
  return os.system(cmd)


def main():
  parser = ArgumentParser()
  parser.add_argument('-x', nargs=1)
  parser.add_argument('--cuda_log', action='store_true')
  parser.add_argument('--use_gcudacc', action='store_true')
  parser.add_argument('--gcudacc_version', action='store', default='v8')
  parser.add_argument('--gcudacc_flag', nargs='*', action='append', default=[])
  args, leftover = parser.parse_known_args(sys.argv[1:])

  if args.x and args.x[0] == 'cuda':
    if args.cuda_log: Log('-x cuda')
    leftover = [pipes.quote(s) for s in leftover]
    if args.use_gcudacc:
      if args.cuda_log: Log('using gcudacc')
      return InvokeGcudacc(argv=leftover,
                           gcudacc_version=args.gcudacc_version,
                           gcudacc_flags=args.gcudacc_flag,
                           log=args.cuda_log)
    if args.cuda_log: Log('using nvcc')
    return InvokeNvcc(leftover, log=args.cuda_log)

  # Strip our flags before passing through to the CPU compiler for files which
  # are not -x cuda. We can't just pass 'leftover' because it also strips -x.
  # We not only want to pass -x to the CPU compiler, but also keep it in its
  # relative location in the argv list (the compiler is actually sensitive to
  # this).
  cpu_compiler_flags = [flag for flag in sys.argv[1:]
                             if not flag.startswith(('--cuda_log',
                                                     '--use_gcudacc',
                                                     '--gcudacc_version',
                                                     '--gcudacc_flag'))]
  if args.use_gcudacc:
    # This macro is defined for TUs that are not marked with "-x cuda" but are
    # built as part of a -config=cuda --use_gcudacc compilation. They are
    # compiled with the default CPU compiler. Since the objects built from
    # these TUs are later linked with objects that come from gcudacc, some
    # parts of the code need to be marked for these special cases. For example,
    # some types have to be defined similarly for gcudacc-compiled TUs and
    # default CPU compiler-compiled TUs linked with them, but differently when
    # nvcc is used.
    # TODO(eliben): rename to a more descriptive name.
    cpu_compiler_flags.append('-D__GCUDACC_HOST__')

  return subprocess.call([CPU_COMPILER] + cpu_compiler_flags)

if __name__ == '__main__':
  sys.exit(main())
