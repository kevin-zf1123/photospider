REGISTRY = {}

def op(fn):
    REGISTRY[fn.__name__] = fn
    return fn
