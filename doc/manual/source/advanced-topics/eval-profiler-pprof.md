# Using the `pprof` evaluation profiler

The `pprof` profiler produces profiles in the [pprof protobuf format](https://github.com/google/pprof),
compatible with `go tool pprof` and other pprof-compatible tools. It captures:

- **CPU samples** (which nix functions are called most often)
- **Allocation objects** (which functions allocate the most objects)
- **Allocation space** (which functions allocate the most bytes)
- **Force count** (which functions force the most thunks)

All sample types are captured simultaneously with full nix call stacks.

## Basic usage

```console
$ nix-instantiate \
    --eval-profiler pprof \
    --eval-profile-file profile.pb \
    --expr '...'
```

Then view interactively:

```console
$ go tool pprof -http=:8080 profile.pb
```

Or from the command line:

```console
$ go tool pprof -top -sample_index=alloc_space profile.pb
```

## Configuration

| Setting | Default | Description |
|---------|---------|-------------|
| `eval-profiler` | `disabled` | Set to `pprof` to enable |
| `eval-profile-file` | `nix.profile` | Output file path |
| `eval-profiler-sample-interval` | `1024` | Function calls between samples (higher = less overhead) |

The sample interval controls the tradeoff between overhead and resolution.
Use `1` for maximum detail (high overhead), `1024` (default) for ~13% overhead,
or `4096`+ for minimal overhead.

## Example: profiling a NixOS configuration

```console
$ nix-instantiate \
    --eval-profiler pprof \
    --eval-profiler-sample-interval 1024 \
    --eval-profile-file nixos-profile.pb \
    -I nixpkgs=flake:nixpkgs \
    --eval \
    --expr 'let nixos = import <nixpkgs/nixos> {
      configuration = {
        boot.loader.grub.device = "/dev/sda";
        fileSystems."/".device = "/dev/sda1";
        services.openssh.enable = true;
        services.nginx.enable = true;
        networking.firewall.enable = true;
      };
    }; in builtins.length (builtins.attrNames nixos.config)'
```

View allocation hotspots:

```console
$ go tool pprof -top -sample_index=alloc_space nixos-profile.pb
Type: alloc_space
Duration: 8.92s, Total samples = 108.38MB
      flat  flat%   sum%        cum   cum%
   17.02MB 15.70% 15.70%    17.02MB 15.70%  lambda@lib/attrsets.nix:1343:41
    9.41MB  8.69% 24.39%    28.59MB 26.38%  primop mapAttrs
    3.48MB  3.21% 33.52%    36.37MB 33.56%  primop concatMap
    3.36MB  3.10% 36.62%     3.47MB  3.20%  optional
    3.29MB  3.04% 39.66%     6.61MB  6.10%  isFunction
```

View full call stacks:

```console
$ go tool pprof -traces -sample_index=samples nixos-profile.pb
         8   lambda@lib/modules.nix:812:11
             primop all
             primop map
             primop zipAttrsWith
             primop length
             primop addErrorContext
             lambda@lib/modules.nix:283:68
             lambda@lib/attrsets.nix:1187:17
```

## Heap snapshots

While evaluation is running, you can take a point-in-time heap snapshot
showing what is currently alive on the heap, attributed to the nix call
stacks that allocated it.

### Triggering a heap snapshot

Create a trigger file in the same directory as the profile output:

```console
$ touch /tmp/.nix-heap-snapshot-trigger
```

The profiler checks for this file every 100ms. When found, it removes
the file, forces a full GC, walks the live heap, and writes a snapshot
to `<profile-dir>/nix-heap-<timestamp>.pb`.

### Example

In one terminal, run a long evaluation:

```console
$ nix-instantiate \
    --eval-profiler pprof \
    --eval-profiler-sample-interval 1024 \
    --eval-profile-file /tmp/profile.pb \
    -I nixpkgs=flake:nixpkgs \
    --eval --strict \
    --expr 'let nixos = import <nixpkgs/nixos> {
      configuration = { ... };
    }; in nixos.config'
```

In another terminal, trigger the snapshot:

```console
$ touch /tmp/.nix-heap-snapshot-trigger
```

Then view the heap snapshot:

```console
$ go tool pprof -top -sample_index=inuse_space /tmp/nix-heap-*.pb
Type: inuse_space
Showing nodes accounting for 4356.82kB, 100% of 4356.82kB total
      flat  flat%   sum%        cum   cum%
  639.83kB 14.69% 14.69%   706.62kB 16.22%  lambda@lib/lists.nix:347:29
  368.36kB  8.45% 23.14%  4356.82kB   100%  primop length
  367.94kB  8.45% 31.59%   367.94kB  8.45%  collectResults
  279.51kB  6.42% 38.00%  4356.82kB   100%  primop concatLists
  249.40kB  5.72% 62.04%  2107.51kB 48.37%  lambda@lib/modules.nix:455:11
  244.72kB  5.62% 67.66%   761.67kB 17.48%  applyModuleArgs
  136.33kB  3.13% 79.26%  1858.10kB 42.65%  loadModule
```

This shows which nix functions are responsible for the memory currently
alive on the GC heap.

## Available sample types

Use `-sample_index=<type>` with `go tool pprof`:

| Sample type | Description |
|-------------|-------------|
| `samples` | CPU samples (which functions are active) |
| `alloc_objects` | Number of objects allocated |
| `alloc_space` | Bytes allocated |
| `force` | Number of thunk forces |
| `inuse_objects` | Live objects on heap (heap snapshots only) |
| `inuse_space` | Live bytes on heap (heap snapshots only) |

## Other profiler modes

| Mode | Output format | Use case |
|------|--------------|----------|
| `flamegraph` | Folded stacks (text) | Quick flamegraph.pl visualization |
| `allocs` | Folded stacks (text) | Allocation-only flamegraph |
| `pprof` | Protobuf binary | Interactive exploration, multiple sample types, heap snapshots |
