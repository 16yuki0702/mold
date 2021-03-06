# mold: A Modern Linker

![mold image](mold.jpg)

This is a repository of a linker I'm currently developing as an
independent project for my Masters degree.

My goal is to make a linker that is as fast as concatenating input
object files with `cat` command. It may sound like an impossible goal,
but I believe it's not entirely impossible because of the following
two reasons:

1. `cat` is a simple single-threaded program which isn't the fastest
   one as a file copy command. My linker can use multiple threads to
   copy file contents more efficiently to save time to do extra work.

2. Copying file contents is I/O-bounded, and many CPU cores should be
   available during file copy. We can use them to do extra work while
   copying file contents.

Concretely speaking, I want to use the linker to link a Chromium
executable (~1.8 GiB in size) just in 1 second. LLVM's lld, the
fastest open-source linker which I originally created a few years ago,
takes about 12 seconds to link Chromium on my machine. So the goal is
12x performance bump over lld. Compared to GNU gold, it's more than
50x.

I don't know if I can ever achieve that, but it's worth a try. I need
to create something anyway to earn units to graduate, and I want to
(at least try to) create something useful.

I have quite a few new ideas as to how to achieve that speedup, though
they are still just random unproved thoughts which need to be
implemented and tested with benchmarks. Here is a brain dump:

## Background

- Even though lld has significantly improved the situation, linking is
  still one of the slowest steps in a build. It is especially
  annoying when I changed one line of code and had to wait for a few
  seconds or even more for a linker to complete. It should be
  instantaneous. There's a need for a faster linker.

- The number of cores on a PC has increased a lot lately, and this
  trend is expected to continue. However, the existing linkers can't
  take the advantage of that because they don't scale well for more
  cores. I have a 64-core/128-thread machine, so my goal is to create
  a linker that uses the CPU nicely. mold should be much faster than
  other linkers on 4 or 8-core machines too, though.

- It looks to me that the designs of the existing linkers are somewhat
  similar, and I believe there are a lot of drastically different
  designs that haven't been explored yet. Develoeprs generally don't
  care about linkers as long as they work correctly, and they don't
  even think about creating a new one. So there may be lots of low
  hanging fruits there in this area.

## Basic design

- In order to achieve a `cat`-like performance, the most important
  thing is to fix the layout of an output file as quickly as possible, so
  that we can start copying actual data from input object files to an
  output file as soon as possible.

- Copying data from input files to an output file is I/O-bounded, so
  there should be room for doing computationally-intensive tasks while
  copying data from one file to another.

- After the first invocation of the linker, the linker should not exit
  but instead become a daemon to keep parsed input files in memory.
  Subsequent linker invocations for the same output file make the
  linker daemon to reload updated input files, and then the daemon
  calls fork(2) to create a subprocess and let it do the actual linking.

- Daemonizing alone wouldn't make the linker magically faster. We need
  to split the linker into two in such a way that the latter half of
  the process finishes as quickly as possible by speculatively parsing
  and preprocessing input files in the first half of the process. The
  key factor of success would be to design nice data structures that
  allows us to offload as much processing as possible from the second
  to the first half.

- One of the most time-consuming stage among linker stages is symbol
  resolution. To resolve symbols, we basically have to throw all
  symbol strings into a hash table to match undefined symbols with
  defined symbols. But this can be done in the daemon using [string
  interning](https://en.wikipedia.org/wiki/String_interning).

- Object files may contain a special section called a mergeable string
  section. The section contains lots of null-terminated strings, and
  the linker is expected to gather all mergeable string sections and
  merge their contents. So, if two object files contain the same
  string literal, for example, the resulting output will contain a
  single merged string. This step is time-consuming, but string
  merging can be done in the daemon using string interning.

- Static archives (.a files) contain object files, but the static
  archive's string table contains only defined symbols of member
  object files and lacks other types of symbols. That makes static
  archives unsuitable for speculative parsing. The daemon should
  ignore the string table of static archive and directly read all
  member object files of all archives to get the whole picture of
  all possible input files.

- If there's a relocation that uses a GOT of a symbol, then we have to
  create a GOT entry for that symbol. Otherwise, we shouldn't. That
  means we need to scan all relocation tables to fix the length and
  the contents of a .got section. This is perhaps time-consuming, but
  we can do that while copying data from input files to an output
  file. After the data copy is done, we can attach a .got section at
  the end of the output file.

- Many linkers support incremental linking, but I think that's a hack
  to work around the slowness of regular linking. I want to focus on
  making the regular linking extremely fast.

## Compatibility

- GNU ld, GNU gold and LLVM lld support essentially the same set of
  command line options and features. mold doesn't have to be
  completely compatible with them. As long as it can be used for
  linking large user-land programs, I'm fine with that. It is OK to
  leave some command line options unimplemented; if mold is blazingly
  fast, other projects would still be happy to adopt it by modifying
  their projects' build files.

- I don't want to support the linker script language in mold because
  it's so complicated and inevitably slows down the linker. User-land
  programs rarely use linker scripts, so it shouldn't be a roadblock
  for most projects.

- mold emits Linux executables and runs only on Linux. I won't avoid
  Unix-ism when writing code (e.g. I'll probably use fork(2)).
  I don't want to think about portability until mold becomes a thing
  that's worth to be ported.

## Details

- If we aim to the 1 second goal for Chromium, every millisecond
  counts. We can't ignore the latency of process exit. If we mmap a
  lot of files, \_exit(2) is not instantaneous but takes a few hundred
  milliseconds because the kernel has to clean up a lot of
  resources. As a workaround, we should organize the linker command as
  two processes; the first process forks the second process, and the
  second process does the actual work. As soon as the second process
  writes a result file to a filesystem, it notifies the first process,
  and the first process exits. The second process can take time to
  exit, because it is not an interactive process.

- At least on Linux, it looks like the filesystem's performance to
  allocate new blocks to a new file is the limiting factor when
  creating a new large file and filling its contents using mmap.
  If you already have a large file on a filesystem, writing to it is
  much faster than creating a new fresh file and writing to it.
  Based on this observation, mold should overwrite to an existing
  executable file if exists. My quick benchmark showed that I could
  save 300 milliseconds when creating a 2 GiB output file.

  Linux doesn't allow to open an executable for writing if it is
  running (you'll get "text busy" error if you attempt). mold should
  fall back to the usual way if it fails to open an output file.

- The output from the linker should be deterministic for the sake of
  [build reproducibility](https://en.wikipedia.org/wiki/Reproducible_builds)
  and ease of debugging. This might add a little bit of overhead to
  the linker, but that shouldn't be too much.

- A .build-id, a unique ID embedded to an output file, is usually
  computed by applying a cryptographic hash function (e.g. SHA-1) to
  an output file. But it adds an extra time for linking because a
  linker has to compute a SHA-1 checksum after the actual linking is
  done. We should instead compute a SHA-1 for the tuple of (all input
  files, command line options, linker version) as a build-id, as it
  should uniquely identify the output.

- [Intel Threading Building
  Blocks](https://github.com/oneapi-src/oneTBB) (TBB) is a good
  library for parallel execution and has several concurrent
  containers. We are particularly interested in using
  `parallel_for_each` and `concurrent_hash_map`.

## Size of the problem

When linking Chrome, a linker reads 3,430,966,844 bytes of data in
total. The data contains the following items:

| Data item                | Number
| ------------------------ | ------
| Object files             | 30,723
| Public undefined symbols | 1,428,149
| Mergeable strings        | 1,579,996
| Comdat groups            | 9,914,510
| Regular sections (*1)    | 10,345,314
| Public defined symbols   | 10,512,135
| Symbols                  | 23,953,607
| Sections                 | 27,543,225
| Relocations against SHF_ALLOC sections | 39,496,375
| Relocations              | 62,024,719

(*1) Sections that have to be copied from input object files to an
output file. Sections that contain relocations or symbols are for
example excluded.
