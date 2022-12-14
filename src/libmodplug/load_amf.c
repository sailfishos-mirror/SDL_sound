/*
 * This source code is public domain.
 *
 * Authors: Olivier Lapicque <olivierl@jps.net>
*/

///////////////////////////////////////////////////
//
// AMF module loader
//
// There is 2 types of AMF files:
// - ASYLUM Music Format
// - Advanced Music Format(DSM)
//
///////////////////////////////////////////////////
#include "libmodplug.h"

#pragma pack(1)

typedef struct _AMFFILEHEADER
{
	UCHAR szAMF[3];
	UCHAR version;
	CHAR title[32];
	UCHAR numsamples;
	UCHAR numorders;
	USHORT numtracks;
	UCHAR numchannels;
} AMFFILEHEADER;

typedef struct _AMFSAMPLE
{
	UCHAR type;
	CHAR  samplename[32];
	CHAR  filename[13];
	ULONG offset;
	ULONG length;
	USHORT c2spd;
	UCHAR volume;
} AMFSAMPLE;

#pragma pack()


static VOID AMF_Unpack(MODCOMMAND *pPat, const BYTE *pTrack, UINT nRows, UINT nChannels)
//-------------------------------------------------------------------------------
{
	UINT lastinstr = 0;
	UINT nTrkSize = bswapLE16(*(USHORT *)pTrack);
	nTrkSize += (UINT)pTrack[2] << 16;
	pTrack += 3;
	while (nTrkSize--)
	{
		UINT row = pTrack[0];
		UINT cmd = pTrack[1];
		UINT arg = pTrack[2];
		MODCOMMAND *m;
		if (row >= nRows) break;
		m = pPat + row * nChannels;
		if (cmd < 0x7F) // note+vol
		{
			m->note = cmd+1;
			if (!m->instr) m->instr = lastinstr;
			m->volcmd = VOLCMD_VOLUME;
			m->vol = arg;
		} else
		if (cmd == 0x7F) // duplicate row
		{
			signed char rdelta = (signed char)arg;
			int rowsrc = (int)row + (int)rdelta;
			if ((rowsrc >= 0) && (rowsrc < (int)nRows)) SDL_memcpy(m, &pPat[rowsrc*nChannels],sizeof(pPat[rowsrc*nChannels]));
		} else
		if (cmd == 0x80) // instrument
		{
			m->instr = arg+1;
			lastinstr = m->instr;
		} else
		if (cmd == 0x83) // volume
		{
			m->volcmd = VOLCMD_VOLUME;
			m->vol = arg;
		} else
		// effect
		{
			UINT command = cmd & 0x7F;
			UINT param = arg;
			switch(command)
			{
			// 0x01: Set Speed
			case 0x01:	command = CMD_SPEED; break;
			// 0x02: Volume Slide
			// 0x0A: Tone Porta + Vol Slide
			// 0x0B: Vibrato + Vol Slide
			case 0x02:	command = CMD_VOLUMESLIDE;
			case 0x0A:	if (command == 0x0A) command = CMD_TONEPORTAVOL;
			case 0x0B:	if (command == 0x0B) command = CMD_VIBRATOVOL;
						if (param & 0x80) param = (-(signed char)param)&0x0F;
						else param = (param&0x0F)<<4;
						break;
			// 0x04: Porta Up/Down
			case 0x04:	if (param & 0x80) { command = CMD_PORTAMENTOUP; param = (-(signed char)param)&0x7F; }
						else { command = CMD_PORTAMENTODOWN; } break;
			// 0x06: Tone Portamento
			case 0x06:	command = CMD_TONEPORTAMENTO; break;
			// 0x07: Tremor
			case 0x07:	command = CMD_TREMOR; break;
			// 0x08: Arpeggio
			case 0x08:	command = CMD_ARPEGGIO; break;
			// 0x09: Vibrato
			case 0x09:	command = CMD_VIBRATO; break;
			// 0x0C: Pattern Break
			case 0x0C:	command = CMD_PATTERNBREAK; break;
			// 0x0D: Position Jump
			case 0x0D:	command = CMD_POSITIONJUMP; break;
			// 0x0F: Retrig
			case 0x0F:	command = CMD_RETRIG; break;
			// 0x10: Offset
			case 0x10:	command = CMD_OFFSET; break;
			// 0x11: Fine Volume Slide
			case 0x11:	if (param) { command = CMD_VOLUMESLIDE;
							if (param & 0x80) param = 0xF0|((-(signed char)param)&0x0F);
							else param = 0x0F|((param&0x0F)<<4);
						} else command = 0; break;
			// 0x12: Fine Portamento
			// 0x16: Extra Fine Portamento
			case 0x12:
			case 0x16:	if (param) { int mask = (command == 0x16) ? 0xE0 : 0xF0;
							command = (param & 0x80) ? CMD_PORTAMENTOUP : CMD_PORTAMENTODOWN;
							if (param & 0x80) param = mask|((-(signed char)param)&0x0F);
							else param |= mask;
						} else command = 0; break;
			// 0x13: Note Delay
			case 0x13:	command = CMD_S3MCMDEX; param = 0xD0|(param & 0x0F); break;
			// 0x14: Note Cut
			case 0x14:	command = CMD_S3MCMDEX; param = 0xC0|(param & 0x0F); break;
			// 0x15: Set Tempo
			case 0x15:	command = CMD_TEMPO; break;
			// 0x17: Panning
			case 0x17:	param = (param+64)&0x7F;
						if (m->command) { if (!m->volcmd) { m->volcmd = VOLCMD_PANNING;  m->vol = param/2; } command = 0; }
						else { command = CMD_PANNING8; }
				break;
			// Unknown effects
			default:	command = param = 0;
			}
			if (command)
			{
				m->command = command;
				m->param = param;
			}
		}
		pTrack += 3;
	}
}


BOOL CSoundFile_ReadAMF(CSoundFile *_this, LPCBYTE lpStream, const DWORD dwMemLength)
//-----------------------------------------------------------
{
	const AMFFILEHEADER *pfh = (AMFFILEHEADER *)lpStream;
	DWORD dwMemPos;
	UINT i;
	
	if ((!lpStream) || (dwMemLength < 2048)) return FALSE;
	if ((!SDL_strncmp((LPCSTR)lpStream, "ASYLUM Music Format V1.0", 25)) && (dwMemLength > 4096))
	{
		UINT numorders, numpats, numsamples;

		dwMemPos = 32;
		numpats = lpStream[dwMemPos+3];
		numorders = lpStream[dwMemPos+4];
		numsamples = 64;
		dwMemPos += 6;
		if ((!numpats) || (numpats > MAX_PATTERNS) || (!numorders)
		 || (numpats*64*32 + 294 + 37*64 >= dwMemLength)) return FALSE;
		_this->m_nType = MOD_TYPE_AMF0;
		_this->m_nChannels = 8;
		_this->m_nInstruments = 0;
		_this->m_nSamples = 31;
		_this->m_nDefaultTempo = 125;
		_this->m_nDefaultSpeed = 6;
		for (i=0; i<MAX_ORDERS; i++)
		{
			_this->Order[i] = (i < numorders) ? lpStream[dwMemPos+i] : 0xFF;
		}
		dwMemPos = 294; // ???
		for (i=0; i<numsamples; i++)
		{
			MODINSTRUMENT *psmp = &_this->Ins[i+1];
			psmp->nFineTune = MOD2XMFineTune(lpStream[dwMemPos+22]);
			psmp->nVolume = lpStream[dwMemPos+23];
			psmp->nGlobalVol = 64;
			if (psmp->nVolume > 0x40) psmp->nVolume = 0x40;
			psmp->nVolume <<= 2;
			psmp->nLength = bswapLE32(*((LPDWORD)(lpStream+dwMemPos+25)));
			psmp->nLoopStart = bswapLE32(*((LPDWORD)(lpStream+dwMemPos+29)));
			psmp->nLoopEnd = psmp->nLoopStart + bswapLE32(*((LPDWORD)(lpStream+dwMemPos+33)));
			if ((psmp->nLoopEnd > psmp->nLoopStart) && (psmp->nLoopEnd <= psmp->nLength))
			{
				psmp->uFlags = CHN_LOOP;
			} else
			{
				psmp->nLoopStart = psmp->nLoopEnd = 0;
			}
			if ((psmp->nLength) && (i>31)) _this->m_nSamples = i+1;
			dwMemPos += 37;
		}
		for (i=0; i<numpats; i++)
		{
			MODCOMMAND *p = CSoundFile_AllocatePattern(64, _this->m_nChannels);
			const UCHAR *pin;
			UINT j;
			if (!p) break;
			_this->Patterns[i] = p;
			_this->PatternSize[i] = 64;
			pin = lpStream + dwMemPos;
			for (j=0; j<8*64; j++)
			{
				p->note = 0;

				if (pin[0])
				{
					p->note = pin[0] + 13;
				}
				p->instr = pin[1];
				p->command = pin[2];
				p->param = pin[3];
				if (p->command > 0x0F)
				{
					p->command = 0;
				}
				CSoundFile_ConvertModCommand(_this, p);
				pin += 4;
				p++;
			}
			dwMemPos += 64*32;
		}
		// Read samples
		for (i=0; i<_this->m_nSamples; i++)
		{
			MODINSTRUMENT *psmp = &_this->Ins[i+1];
			if (psmp->nLength)
			{
				if (dwMemPos > dwMemLength) return FALSE;
				dwMemPos += CSoundFile_ReadSample(_this, psmp, RS_PCM8S, (LPCSTR)(lpStream+dwMemPos), dwMemLength-dwMemPos);
			}
		}
		return TRUE;
	}
    else			/**/
    {
	////////////////////////////
	// DSM/AMF
	USHORT *ptracks[MAX_PATTERNS];
	DWORD sampleseekpos[MAX_SAMPLES];
	USHORT *pTrackMap;
	BYTE **pTrackData;
	UINT maxsampleseekpos;
	UINT realtrackcnt;

	if ((pfh->szAMF[0] != 'A') || (pfh->szAMF[1] != 'M') || (pfh->szAMF[2] != 'F')
	 || (pfh->version < 10) || (pfh->version > 14) || (!bswapLE16(pfh->numtracks))
	 || (!pfh->numorders) || (pfh->numorders > MAX_PATTERNS)
	 || (!pfh->numsamples) || (pfh->numsamples >= MAX_SAMPLES)
	 || (pfh->numchannels < 4) || (pfh->numchannels > 32))
		return FALSE;
	dwMemPos = sizeof(AMFFILEHEADER);
	_this->m_nType = MOD_TYPE_AMF;
	_this->m_nChannels = pfh->numchannels;
	_this->m_nSamples = pfh->numsamples;
	_this->m_nInstruments = 0;
	// Setup Channel Pan Positions
	if (pfh->version >= 11)
	{
		signed char *panpos = (signed char *)(lpStream + dwMemPos);
		UINT nchannels = (pfh->version >= 13) ? 32 : 16;
		for (i=0; i<nchannels; i++)
		{
			int pan = (panpos[i] + 64) * 2;
			if (pan < 0) pan = 0;
			if (pan > 256) { pan = 128; _this->ChnSettings[i].dwFlags |= CHN_SURROUND; }
			_this->ChnSettings[i].nPan = pan;
		}
		dwMemPos += nchannels;
	} else
	{
		for (i=0; i<16; i++)
		{
			_this->ChnSettings[i].nPan = (lpStream[dwMemPos+i] & 1) ? 0x30 : 0xD0;
		}
		dwMemPos += 16;
	}
	// Get Tempo/Speed
	_this->m_nDefaultTempo = 125;
	_this->m_nDefaultSpeed = 6;
	if (pfh->version >= 13)
	{
		if (lpStream[dwMemPos] >= 32) _this->m_nDefaultTempo = lpStream[dwMemPos];
		if (lpStream[dwMemPos+1] <= 32) _this->m_nDefaultSpeed = lpStream[dwMemPos+1];
		dwMemPos += 2;
	}
	// Setup sequence list
	for (i=0; i<MAX_ORDERS; i++)
	{
		if (dwMemPos + 4 > dwMemLength) return TRUE;
		_this->Order[i] = 0xFF;
		if (i < pfh->numorders)
		{
			_this->Order[i] = i;
			_this->PatternSize[i] = 64;
			if (pfh->version >= 14)
			{
				if (dwMemPos + _this->m_nChannels * sizeof(USHORT) + 2 > dwMemLength) return FALSE;
				_this->PatternSize[i] = bswapLE16(*(USHORT *)(lpStream+dwMemPos));
				dwMemPos += 2;
			} else
			{
				if (dwMemPos + _this->m_nChannels * sizeof(USHORT) > dwMemLength) return FALSE;
			}
			ptracks[i] = (USHORT *)(lpStream+dwMemPos);
			dwMemPos += _this->m_nChannels * sizeof(USHORT);
		}
	}
	if (dwMemPos + _this->m_nSamples * (sizeof(AMFSAMPLE)+8) > dwMemLength) return TRUE;
	// Read Samples
	maxsampleseekpos = 0;
	for (i=0; i<_this->m_nSamples; i++)
	{
		MODINSTRUMENT *pins = &_this->Ins[i+1];
		const AMFSAMPLE *psh = (AMFSAMPLE *)(lpStream + dwMemPos);

		dwMemPos += sizeof(AMFSAMPLE);
		pins->nLength = bswapLE32(psh->length);
		pins->nC4Speed = bswapLE16(psh->c2spd);
		pins->nGlobalVol = 64;
		pins->nVolume = psh->volume * 4;
		if (pfh->version >= 11)
		{
			pins->nLoopStart = bswapLE32(*(DWORD *)(lpStream+dwMemPos));
			pins->nLoopEnd = bswapLE32(*(DWORD *)(lpStream+dwMemPos+4));
			dwMemPos += 8;
		} else
		{
			pins->nLoopStart = bswapLE16(*(WORD *)(lpStream+dwMemPos));
			pins->nLoopEnd = pins->nLength;
			dwMemPos += 2;
		}
		sampleseekpos[i] = 0;
		if ((psh->type) && (bswapLE32(psh->offset) < dwMemLength-1))
		{
			sampleseekpos[i] = bswapLE32(psh->offset);
			if (bswapLE32(psh->offset) > maxsampleseekpos) 
				maxsampleseekpos = bswapLE32(psh->offset);
			if ((pins->nLoopEnd > pins->nLoopStart + 2)
			 && (pins->nLoopEnd <= pins->nLength)) pins->uFlags |= CHN_LOOP;
		}
	}
	// Read Track Mapping Table
	pTrackMap = (USHORT *)(lpStream+dwMemPos);
	realtrackcnt = 0;
	dwMemPos += pfh->numtracks * sizeof(USHORT);
	if (dwMemPos >= dwMemLength)
		return TRUE;

	for (i=0; i<pfh->numtracks; i++)
	{
		if (realtrackcnt < pTrackMap[i]) realtrackcnt = pTrackMap[i];
	}
	// Store tracks positions
	pTrackData = (BYTE **) SDL_calloc(realtrackcnt, sizeof(BYTE*));
	if (!pTrackData) return TRUE;/*FIXME: return FALSE? */
	for (i=0; i<realtrackcnt; i++) if (dwMemPos <= dwMemLength - 3)
	{
		UINT nTrkSize = bswapLE16(*(USHORT *)(lpStream+dwMemPos));
		nTrkSize += (UINT)lpStream[dwMemPos+2] << 16;
		if (dwMemPos + nTrkSize * 3 + 3 <= dwMemLength)
		{
			pTrackData[i] = (BYTE *)(lpStream + dwMemPos);
		}
		dwMemPos += nTrkSize * 3 + 3;
	}
	// Create the patterns from the list of tracks
	for (i=0; i<pfh->numorders; i++)
	{
		MODCOMMAND *p = CSoundFile_AllocatePattern(_this->PatternSize[i], _this->m_nChannels);
		UINT ch;
		if (!p) break;
		_this->Patterns[i] = p;
		for (ch=0; ch<_this->m_nChannels; ch++)
		{
			UINT nTrack = bswapLE16(ptracks[i][ch]);
			if ((nTrack) && (nTrack <= pfh->numtracks))
			{
				UINT realtrk = bswapLE16(pTrackMap[nTrack-1]);
				if (realtrk)
				{
					realtrk--;
					if ((realtrk < realtrackcnt) && (pTrackData[realtrk]))
					{
						AMF_Unpack(p+ch, pTrackData[realtrk], _this->PatternSize[i], _this->m_nChannels);
					}
				}
			}
		}
	}
	SDL_free(pTrackData);
	// Read Sample Data
	for (i=1; i<=maxsampleseekpos; i++)
	{
		UINT smp;
		if (dwMemPos >= dwMemLength) break;
		for (smp=0; smp<_this->m_nSamples; smp++) if (i == sampleseekpos[smp])
		{
			MODINSTRUMENT *pins = &_this->Ins[smp+1];
			dwMemPos += CSoundFile_ReadSample(_this, pins, RS_PCM8U, (LPCSTR)(lpStream+dwMemPos), dwMemLength-dwMemPos);
			break;
		}
	}
	return TRUE;
    }				/**/
}
