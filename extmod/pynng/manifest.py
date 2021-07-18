# This list of frozen files doesn't include task.py because that's provided by the C module.
freeze(
    "..",
    (
        "pynng/__init__.py",
        "pynng/_aio.py",
        "pynng/exceptions.py",
        "pynng/nng.py",
        "pynng/options.py",
        "pynng/sockaddr.py",
        "pynng/tls.py",
        "pynng/_version.py",
    ),
    opt=3,
)
