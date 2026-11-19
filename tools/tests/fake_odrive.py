"""A fake ODrive object tree for tests (no hardware). Records every write and call."""


class _Node(object):
    def __init__(self, root, prefix):
        object.__setattr__(self, "_root", root)
        object.__setattr__(self, "_prefix", prefix)

    def _path(self, name):
        return (self._prefix + "." + name) if self._prefix else name

    def __getattr__(self, name):
        path = self._path(name)
        values = self._root.values
        if path in values:
            self._root.reads.append(path)
            return values[path]
        if any(k.startswith(path + ".") for k in values):
            return _Node(self._root, path)
        raise AttributeError(path)

    def __setattr__(self, name, value):
        path = self._path(name)
        if path not in self._root.values:
            raise AttributeError(path)
        self._root.writes.append((path, value))
        self._root.values[path] = value
        hook = self._root.on_write
        if hook:
            hook(path, value)


class FakeODrive(_Node):
    def __init__(self, values, on_write=None):
        object.__setattr__(self, "values", dict(values))
        object.__setattr__(self, "writes", [])
        object.__setattr__(self, "reads", [])
        object.__setattr__(self, "calls", [])
        object.__setattr__(self, "on_write", on_write)
        _Node.__init__(self, self, "")

    def save_configuration(self):
        self.calls.append("save_configuration")

    def reboot(self):  # pragma: no cover - must never be called
        self.calls.append("reboot")
