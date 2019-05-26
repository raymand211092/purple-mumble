Introduction
============
purple-mumble is a Mumble protocol plugin for libpurple.

Usage
=====
1. Clone and build [latest development version of Pidgin](https://bitbucket.org/pidgin/main/src).
2. Run `make` (or `bear make` to generate `compile_commands.json` for
   clang-tidy that is used by the pre-commit hook) to build the plugin.
3. Run `cp mumble.so $XDG_CONFIG_HOME/gplugin` to install the plugin.
4. Start `pidgin`.
