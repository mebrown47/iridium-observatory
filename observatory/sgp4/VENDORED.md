# Vendored: python-sgp4 2.23 (pure-Python path only)

From https://pypi.org/project/sgp4/2.23/ by Brandon Rhodes, MIT licensed
(see LICENSE). Only the modules the pure-Python `sgp4.model.Satrec` needs are
kept: `__init__ alpha5 earth_gravity ext functions io model propagation`,
unmodified. The Observatory imports `sgp4.model` directly so it never depends
on a system-installed sgp4 or its C extension, and stays stdlib-only.
