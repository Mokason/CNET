"""Independent bounded arithmetic labels; never consumes CNET output."""
import sys
op=sys.argv[1]
for x in range(32 if op=='partial' else 64):
    if op=='left':y=(x+1)&63
    elif op in ('right','partial'):y=(x*2)&63
    elif op=='extra':y=(x+3)&63
    else:raise ValueError('unknown oracle operation')
    print(f'{x}\t{y}')
