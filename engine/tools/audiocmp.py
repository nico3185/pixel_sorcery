#!/usr/bin/env python3
"""audiocmp.py — numeric comparison of two time-aligned WAVs (native synth vs reference).

Never listens: prints level, spectral and pitch metrics only.
  audiocmp.py native.wav ref.wav [--from SEC] [--to SEC] [--worst N]
Metrics (mono mix, 44.1 kHz):
  level     RMS of each, ratio in dB
  lag       best alignment lag (ms) by cross-correlation of onset envelopes
  mel       mean |dB difference| of 64-band log-mel spectra (lower is better; <3 dB ~ close)
  mel_corr  correlation of the log-mel spectrograms (1 = identical shape)
  chroma    mean cosine similarity of chroma (pitch classes; 1 = same notes)
  centroid  mean spectral centroid of each (Hz): brightness
  worst     seconds with the largest mel difference
"""
import argparse
import numpy as np
import librosa


def load(p, a, b):
    y, sr = librosa.load(p, sr=None, mono=True, offset=a, duration=(b - a) if b else None)
    return y, sr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('native'); ap.add_argument('ref')
    ap.add_argument('--from', dest='a', type=float, default=0.0)
    ap.add_argument('--to', dest='b', type=float, default=None)
    ap.add_argument('--worst', type=int, default=5)
    o = ap.parse_args()
    x, sr = load(o.native, o.a, o.b); y, _ = load(o.ref, o.a, o.b)
    n = min(len(x), len(y)); x, y = x[:n], y[:n]
    rx, ry = np.sqrt(np.mean(x ** 2)) + 1e-12, np.sqrt(np.mean(y ** 2)) + 1e-12
    print(f'level     native {20*np.log10(rx):6.1f} dBFS  ref {20*np.log10(ry):6.1f} dBFS  ratio {20*np.log10(rx/ry):+.2f} dB')

    hop = 512
    ex = librosa.onset.onset_strength(y=x, sr=sr, hop_length=hop)
    ey = librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop)
    c = np.correlate(ex - ex.mean(), ey - ey.mean(), 'full'); lag = int(np.argmax(c)) - (len(ey) - 1)
    print(f'lag       {lag * hop / sr * 1000:+.1f} ms (onset envelope)')

    # level-normalised so the shape comparison ignores a constant gain difference
    mx = librosa.power_to_db(librosa.feature.melspectrogram(y=x / rx, sr=sr, n_mels=64, hop_length=hop), ref=1.0, top_db=None)
    my = librosa.power_to_db(librosa.feature.melspectrogram(y=y / ry, sr=sr, n_mels=64, hop_length=hop), ref=1.0, top_db=None)
    mx = np.maximum(mx, -60); my = np.maximum(my, -60)
    # only frames where the reference has sound: silence floors would dominate otherwise
    act = np.sqrt(np.mean(librosa.util.frame(np.pad(y, (1024, 1024)), frame_length=2048, hop_length=hop) ** 2, axis=0))[:mx.shape[1]] > 0.003
    mx[:, ~act] = my[:, ~act]
    print(f'active    {act.mean()*100:.0f}% of frames have sound')
    d = np.abs(mx - my)
    print(f'mel       mean |diff| {d[:, act].mean() if act.any() else 0:.2f} dB   corr {np.corrcoef(mx.ravel(), my.ravel())[0,1]:.4f}')

    fr = librosa.mel_frequencies(n_mels=64, fmax=sr / 2)
    prof = (mx - my)[:, act].mean(1) if act.any() else np.zeros(64)
    print('bands     ' + ' '.join(f'{fr[i]/1000:.1f}k:{prof[i:i+8].mean():+.1f}' for i in range(0, 64, 8)) + '  (dB native-ref)')
    cx = librosa.feature.chroma_stft(y=x, sr=sr, hop_length=hop); cy = librosa.feature.chroma_stft(y=y, sr=sr, hop_length=hop)
    loud = (np.sqrt(np.mean(librosa.util.frame(y, frame_length=2048, hop_length=hop) ** 2, axis=0)) > 0.01)[:cx.shape[1]]
    cos = (cx * cy).sum(0) / (np.linalg.norm(cx, axis=0) * np.linalg.norm(cy, axis=0) + 1e-9)
    print(f'chroma    cosine {cos[:len(loud)][loud].mean() if loud.any() else float("nan"):.4f} (frames with sound)')

    sx = librosa.feature.spectral_centroid(y=x, sr=sr, hop_length=hop).mean(); sy = librosa.feature.spectral_centroid(y=y, sr=sr, hop_length=hop).mean()
    print(f'centroid  native {sx:7.0f} Hz  ref {sy:7.0f} Hz')

    per_s = sr // hop
    secs = d.mean(0)[: (d.shape[1] // per_s) * per_s].reshape(-1, per_s).mean(1)
    worst = np.argsort(secs)[::-1][: o.worst]
    print('worst     ' + '  '.join(f'{o.a + s:.0f}s:{secs[s]:.1f}dB' for s in sorted(worst)))


if __name__ == '__main__':
    main()
