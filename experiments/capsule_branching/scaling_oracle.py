"""Independent exhaustive leaf labels for scaling fixtures; no CNET inputs."""
import sys

index = int(sys.argv[1])
assert 0 <= index < 32
for x in range(64):
    print(f'{x}\t{((2*(index % 8)+1)*x+(3*index+1)) % 64}')
