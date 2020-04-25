A minimal unprivileged notifier for system-wide resource pressure using
[PSI](https://facebookmicrosites.github.io/psi/).

## Requirements

- Linux 4.20+
- libnotify

## Config

Put your configuration in ~/.config/psi-notify. Here's an example that will
check roughly every 10 seconds⁺, and pop up a notification when the values are
exceeded:

```
update 10

threshold cpu some avg10 40.00
threshold memory some avg10 25.00
threshold io some avg60 25.00
```

⁺ PSI has poll() support, but it's not currently available to unprivileged
users. See [this
discussion](https://lore.kernel.org/lkml/20200424153859.GA1481119@chrisdown.name).
