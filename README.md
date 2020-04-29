psi-notify is a minimal unprivileged notifier for system-wide resource pressure
using [PSI](https://facebookmicrosites.github.io/psi/).

This can help you to identify misbehaving applications on your machine before
they start to severely impact system responsiveness.

## Features

- Runs unprivileged
- Low resource usage:
   - Anonymous memory usage typically less than 40kB
   - Almost zero CPU usage
- Config reload without restart with `SIGHUP`

## Requirements

- Linux 4.20+
- libnotify

## Config

Put your configuration in `~/.config/psi-notify`. Here's an example that will
check roughly every 5 seconds⁺, and pop up a notification when the values are
exceeded:

```
update 5

threshold cpu some avg10 50.00
threshold memory some avg10 10.00
threshold io some avg10 10.00
```

The above is the default configuration if no config file exists. You may likely
have to tweak these depending on your hardware, machine behaviour, and normal
workloads.

⁺ PSI has poll() support, but it's not currently available to unprivileged
users. See [this
discussion](https://lore.kernel.org/lkml/20200424153859.GA1481119@chrisdown.name).

## Format

The update interval in seconds is specified with `update [int]`. The default is
`update 5` if unspecified.

Thresholds are specified with fields in the following format:

1. The word `threshold`.
2. The resource name, as shown in `cgroup.controllers`. `cpu`, `memory`, and
   `io` are currently supported.
3. Whether to use the `some` or `full` metric. See the definition
   [here](https://facebookmicrosites.github.io/psi/docs/overview#pressure-metric-definitions).
4. The PSI time period. `avg10`, `avg60`, and `avg300` are currently supported.
5. The threshold, as a real number between 0 and 100. Decimals are ok.
