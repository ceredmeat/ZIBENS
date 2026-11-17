# Builds zibens' tables.h: a sine table and the time/frequency curves of its knobs.
import sys, math
out = sys.argv[1]
BLOCK = 1500.0   # 32-sample blocks a second at 48 kHz

def t60_mult(t):  # per-block multiplier, Q16, that falls 60 dB in t seconds
    return min(65535, int(round(65536 * math.exp(-6.9078 / (t * BLOCK)))))

with open(out, 'w') as f:
    f.write('/* Made by gen_zibens.py. Do not edit by hand. */\n')
    f.write('static const int16_t zb_sin[1024] = {' +
            ','.join(str(int(round(32767 * math.sin(2 * math.pi * i / 1024)))) for i in range(1024)) + '};\n')
    dec = [0.008 * 500 ** (v / 127) for v in range(128)]           # DEC: 8 ms .. 4 s
    f.write('static const uint16_t zb_dec_mult[128] = {' + ','.join(str(t60_mult(t)) for t in dec) + '};\n')
    f.write('static const uint16_t zb_dec_ms[128] = {' + ','.join(str(int(round(t * 1000))) for t in dec) + '};\n')
    pdec = [0.002 * 200 ** (v / 127) for v in range(128)]          # PDEC: 2 ms .. 400 ms
    f.write('static const uint16_t zb_pdec_mult[128] = {' + ','.join(str(t60_mult(t)) for t in pdec) + '};\n')
    f.write('static const uint16_t zb_pdec_ms[128] = {' + ','.join(str(int(round(t * 1000))) for t in pdec) + '};\n')
    fc = [40 * 300 ** (v / 127) for v in range(128)]               # cutoffs: 40 Hz .. 12 kHz
    # Chamberlin coefficient 2 sin(pi fc / fs), Q15, kept stable
    f.write('static const uint16_t zb_fc_coef[128] = {' +
            ','.join(str(min(30000, int(round(32768 * 2 * math.sin(math.pi * min(c, 9000) / 48000))))) for c in fc) + '};\n')
    f.write('static const uint16_t zb_fc_hz[128] = {' + ','.join(str(int(round(c))) for c in fc) + '};\n')
    # the same cutoffs at 24 kHz (the voices run at half rate), kept stable
    f_coef24 = [min(30000, int(round(32768 * 2 * math.sin(math.pi * min(c, 4500) / 24000)))) for c in fc]
    f.write('static const uint16_t zb_fc24[128] = {' + ','.join(str(x) for x in f_coef24) + '};\n')
print('ok')
