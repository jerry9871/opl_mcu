/*
    Windows WaveOut backend for audio.h.

    Simple ring of N small buffers. We block in audio_write() whenever no
    buffer is free, so the caller's render rate is naturally throttled to
    real time.
*/
#include "audio.h"

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

#define NUM_BUFFERS   4
#define FRAMES_PER_BUF 1024            /* ~21 ms at 48 kHz */
#define BYTES_PER_BUF (FRAMES_PER_BUF * 2 /*ch*/ * sizeof(int16_t))

static HWAVEOUT  g_hwo;
static HANDLE    g_event;
static WAVEHDR   g_hdr[NUM_BUFFERS];
static int16_t   g_buf[NUM_BUFFERS][FRAMES_PER_BUF * 2];
static unsigned  g_next;               /* next buffer index to fill */
static uint32_t  g_fill;               /* frames already accumulated in g_buf[g_next] */

static void CALLBACK
wave_cb(HWAVEOUT hwo, UINT msg, DWORD_PTR inst,
		DWORD_PTR p1, DWORD_PTR p2)
{
	(void)hwo;
	(void)inst;
	(void)p1;
	(void)p2;

	if (msg == WOM_DONE)
		SetEvent(g_event);
}

int
audio_open(uint32_t sample_rate)
{
	WAVEFORMATEX wf = {0};
	wf.wFormatTag      = WAVE_FORMAT_PCM;
	wf.nChannels       = 2;
	wf.nSamplesPerSec  = sample_rate;
	wf.wBitsPerSample  = 16;
	wf.nBlockAlign     = wf.nChannels * wf.wBitsPerSample / 8;
	wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

	g_event = CreateEvent(NULL, FALSE, FALSE, NULL);

	if (!g_event)
		return 1;

	MMRESULT r = waveOutOpen(&g_hwo, WAVE_MAPPER, &wf,
							 (DWORD_PTR)wave_cb, 0, CALLBACK_FUNCTION);

	if (r != MMSYSERR_NOERROR) {
		fprintf(stderr, "waveOutOpen failed: %u\n", r);
		return 2;
	}

	for (int i = 0; i < NUM_BUFFERS; i++) {
		g_hdr[i].lpData          = (LPSTR)g_buf[i];
		g_hdr[i].dwBufferLength  = BYTES_PER_BUF;
		g_hdr[i].dwFlags         = 0;
		waveOutPrepareHeader(g_hwo, &g_hdr[i], sizeof(WAVEHDR));
		g_hdr[i].dwFlags |= WHDR_DONE;   /* mark "free" so first pass uses them */
	}

	g_next = 0;
	g_fill = 0;
	return 0;
}

/* Submit the buffer at g_next with `frames` valid frames, then advance. */
static void
submit_current(uint32_t frames)
{
	WAVEHDR* h = &g_hdr[g_next];
	h->dwFlags &= ~WHDR_DONE;
	h->dwBufferLength = frames * 2 * sizeof(int16_t);
	waveOutWrite(g_hwo, h, sizeof(WAVEHDR));
	g_next = (g_next + 1) % NUM_BUFFERS;
	g_fill = 0;
}

void
audio_write(const int16_t* src, uint32_t frames)
{
	while (frames) {
		WAVEHDR* h = &g_hdr[g_next];

		/* Wait until this slot has been returned by the device. */
		while (!(h->dwFlags & WHDR_DONE))
			WaitForSingleObject(g_event, INFINITE);

		uint32_t room = FRAMES_PER_BUF - g_fill;
		uint32_t n    = frames < room ? frames : room;

		memcpy(g_buf[g_next] + g_fill * 2, src, n * 2 * sizeof(int16_t));
		g_fill += n;
		src    += n * 2;
		frames -= n;

		if (g_fill == FRAMES_PER_BUF)
			submit_current(FRAMES_PER_BUF);
	}
}

void
audio_close(void)
{
	if (!g_hwo)
		return;

	/* Flush any partially-filled buffer first. */
	if (g_fill) {
		WAVEHDR* h = &g_hdr[g_next];

		while (!(h->dwFlags & WHDR_DONE))
			WaitForSingleObject(g_event, INFINITE);

		submit_current(g_fill);
	}

	/* Wait for every buffer to drain. */
	for (int i = 0; i < NUM_BUFFERS; i++) {
		while (!(g_hdr[i].dwFlags & WHDR_DONE))
			WaitForSingleObject(g_event, INFINITE);

		waveOutUnprepareHeader(g_hwo, &g_hdr[i], sizeof(WAVEHDR));
	}

	waveOutClose(g_hwo);
	CloseHandle(g_event);
	g_hwo = NULL;
	g_event = NULL;
}
