"""Record the timer boundary reported by each numeric benchmark executable."""
import json
import subprocess


def timing_scopes(executable):
    result = subprocess.run([executable, '--timing-scopes'], check=False,
                            text=True, capture_output=True, timeout=10)
    if result.returncode:
        # Older executables do not advertise their timer boundary. Keep that
        # absence visible rather than treating their `core` layer as a match.
        return {layer: 'unknown' for layer in ('public', 'core', 'raw')}
    scopes = json.loads(result.stdout)
    if not isinstance(scopes, dict) or any(
            not isinstance(scopes.get(layer), str) or not scopes[layer]
            for layer in ('public', 'core', 'raw')):
        raise ValueError('benchmark must report all three timing scopes')
    return scopes
