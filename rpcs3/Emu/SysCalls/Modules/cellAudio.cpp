#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/SysCalls/Modules.h"

#include "rpcs3/Ini.h"
#include "Utilities/SQueue.h"
#include "Emu/Event.h"
#include "Emu/SysCalls/lv2/sys_time.h"
#include "Emu/Audio/AudioManager.h"
#include "Emu/Audio/AudioDumper.h"
#include "Emu/Audio/cellAudio.h"

Module *cellAudio = nullptr;

static std::mutex audioMutex;

AudioConfig m_config;

static const bool g_is_u16 = Ini.AudioConvertToU16.GetValue();

// libaudio Functions

#define BUFFER_NUM 32
#define BUFFER_SIZE 256
int cellAudioInit()
{
	cellAudio->Warning("cellAudioInit()");

	if (m_config.m_is_audio_initialized)
	{
		return CELL_AUDIO_ERROR_ALREADY_INIT;
	}

	m_config.m_is_audio_initialized = true;
	m_config.start_time = 0;
	m_config.counter = 0;

	// alloc memory
	m_config.m_buffer = (u32)Memory.Alloc(128 * 1024 * m_config.AUDIO_PORT_COUNT, 1024); 
	memset(vm::get_ptr<void>(m_config.m_buffer), 0, 128 * 1024 * m_config.AUDIO_PORT_COUNT);
	m_config.m_indexes = (u32)Memory.Alloc(sizeof(u64) * m_config.AUDIO_PORT_COUNT, 16);
	memset(vm::get_ptr<void>(m_config.m_indexes), 0, sizeof(u64) * m_config.AUDIO_PORT_COUNT);

	thread t("Audio Thread", []()
		{
			AudioDumper m_dump(8); // WAV file header (8 ch)

			bool do_dump = Ini.AudioDumpToFile.GetValue();
		
			if (do_dump && !m_dump.Init())
			{
				cellAudio->Error("cellAudioInit(): AudioDumper::Init() failed");
				return;
			}

			cellAudio->Notice("Audio thread started");

			if (Ini.AudioDumpToFile.GetValue())
				m_dump.WriteHeader();

			float buf2ch[2 * BUFFER_SIZE]; // intermediate buffer for 2 channels
			float buf8ch[8 * BUFFER_SIZE]; // intermediate buffer for 8 channels

			uint oal_buffer_offset = 0;
			const uint oal_buffer_size = 2 * BUFFER_SIZE;

			std::unique_ptr<s16[]> oal_buffer[BUFFER_NUM];
			std::unique_ptr<float[]> oal_buffer_float[BUFFER_NUM];

			for (u32 i = 0; i < BUFFER_NUM; i++)
			{
				oal_buffer[i] = std::unique_ptr<s16[]>(new s16[oal_buffer_size] {} );
				oal_buffer_float[i] = std::unique_ptr<float[]>(new float[oal_buffer_size] {} );
			}

			SQueue<s16*, 31> queue;
			queue.Clear();

			SQueue<float*, 31> queue_float;
			queue_float.Clear();

			std::vector<u64> keys;

			if(m_audio_out)
			{
				m_audio_out->Init();

				// Note: What if the ini value changes?
				if (g_is_u16)
					m_audio_out->Open(oal_buffer[0].get(), oal_buffer_size * sizeof(s16));
				else
					m_audio_out->Open(oal_buffer_float[0].get(), oal_buffer_size * sizeof(float));
			}

			m_config.start_time = get_system_time();

			volatile bool internal_finished = false;

			thread iat("Internal Audio Thread", [oal_buffer_size, &queue, &queue_float, &internal_finished]()
			{
				while (true)
				{
					s16* oal_buffer = nullptr;
					float* oal_buffer_float = nullptr;

					if (g_is_u16)
						queue.Pop(oal_buffer, nullptr);
					else
						queue_float.Pop(oal_buffer_float, nullptr);

					if (g_is_u16)
					{
						if (oal_buffer)
						{
							m_audio_out->AddData(oal_buffer, oal_buffer_size * sizeof(s16));
							continue;
						}
					}
					else
					{
						if (oal_buffer_float)
						{
							m_audio_out->AddData(oal_buffer_float, oal_buffer_size * sizeof(float));
							continue;
						}
					}
					internal_finished = true;
					return;
				}
			});
			iat.detach();

			while (m_config.m_is_audio_initialized)
			{
				if (Emu.IsStopped())
				{
					cellAudio->Warning("Audio thread aborted");
					goto abort;
				}

				const u64 stamp0 = get_system_time();

				// TODO: send beforemix event (in ~2,6 ms before mixing)

				// precise time of sleeping: 5,(3) ms (or 256/48000 sec)
				if (m_config.counter * 256000000 / 48000 >= stamp0 - m_config.start_time)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}

				m_config.counter++;

				const u32 oal_pos = m_config.counter % BUFFER_NUM;

				if (Emu.IsPaused())
				{
					continue;
				}

				bool first_mix = true;

				// mixing:
				for (u32 i = 0; i < m_config.AUDIO_PORT_COUNT; i++)
				{
					if (!m_config.m_ports[i].m_is_audio_port_started) continue;

					AudioPortConfig& port = m_config.m_ports[i];

					const u32 block_size = port.channel * 256;
					const u32 position = port.tag % port.block; // old value
					const u32 buf_addr = m_config.m_buffer + (i * 128 * 1024) + (position * block_size * sizeof(float));

					auto buf = vm::get_ptr<be_t<float>>(buf_addr);

					static const float k = 1.0f; // may be 1.0f
					const float m = port.level;

					if (port.channel == 2)
					{
						if (first_mix)
						{
							for (u32 i = 0; i < (sizeof(buf2ch) / sizeof(float)); i += 2)
							{
								// reverse byte order
								const float left = buf[i + 0] * m;
								const float right = buf[i + 1] * m;

								buf2ch[i + 0] = left;
								buf2ch[i + 1] = right;

								buf8ch[i * 4 + 0] = left;
								buf8ch[i * 4 + 1] = right;
								buf8ch[i * 4 + 2] = 0.0f;
								buf8ch[i * 4 + 3] = 0.0f;
								buf8ch[i * 4 + 4] = 0.0f;
								buf8ch[i * 4 + 5] = 0.0f;
								buf8ch[i * 4 + 6] = 0.0f;
								buf8ch[i * 4 + 7] = 0.0f;
							}
							first_mix = false;
						}
						else
						{
							for (u32 i = 0; i < (sizeof(buf2ch) / sizeof(float)); i += 2)
							{
								const float left = buf[i + 0] * m;
								const float right = buf[i + 1] * m;

								buf2ch[i + 0] += left;
								buf2ch[i + 1] += right;

								buf8ch[i * 4 + 0] += left;
								buf8ch[i * 4 + 1] += right;
							}
						}
					}
					else if (port.channel == 6)
					{
						if (first_mix)
						{
							for (u32 i = 0; i < (sizeof(buf2ch) / sizeof(float)); i += 2)
							{
								const float left = buf[i * 3 + 0] * m;
								const float right = buf[i * 3 + 1] * m;
								const float center = buf[i * 3 + 2] * m;
								const float low_freq = buf[i * 3 + 3] * m;
								const float rear_left = buf[i * 3 + 4] * m;
								const float rear_right = buf[i * 3 + 5] * m;

								const float mid = (center + low_freq) * 0.708f;
								buf2ch[i + 0] = (left + rear_left + mid) * k;
								buf2ch[i + 1] = (right + rear_right + mid) * k;

								buf8ch[i * 4 + 0] = left;
								buf8ch[i * 4 + 1] = right;
								buf8ch[i * 4 + 2] = center;
								buf8ch[i * 4 + 3] = low_freq;
								buf8ch[i * 4 + 4] = rear_left;
								buf8ch[i * 4 + 5] = rear_right;
								buf8ch[i * 4 + 6] = 0.0f;
								buf8ch[i * 4 + 7] = 0.0f;
							}
							first_mix = false;
						}
						else
						{
							for (u32 i = 0; i < (sizeof(buf2ch) / sizeof(float)); i += 2)
							{
								const float left = buf[i * 3 + 0] * m;
								const float right = buf[i * 3 + 1] * m;
								const float center = buf[i * 3 + 2] * m;
								const float low_freq = buf[i * 3 + 3] * m;
								const float rear_left = buf[i * 3 + 4] * m;
								const float rear_right = buf[i * 3 + 5] * m;

								const float mid = (center + low_freq) * 0.708f;
								buf2ch[i + 0] += (left + rear_left + mid) * k;
								buf2ch[i + 1] += (right + rear_right + mid) * k;

								buf8ch[i * 4 + 0] += left;
								buf8ch[i * 4 + 1] += right;
								buf8ch[i * 4 + 2] += center;
								buf8ch[i * 4 + 3] += low_freq;
								buf8ch[i * 4 + 4] += rear_left;
								buf8ch[i * 4 + 5] += rear_right;
							}
						}
					}
					else if (port.channel == 8)
					{
						if (first_mix)
						{
							for (u32 i = 0; i < (sizeof(buf2ch) / sizeof(float)); i += 2)
							{
								const float left = buf[i * 4 + 0] * m;
								const float right = buf[i * 4 + 1] * m;
								const float center = buf[i * 4 + 2] * m;
								const float low_freq = buf[i * 4 + 3] * m;
								const float rear_left = buf[i * 4 + 4] * m;
								const float rear_right = buf[i * 4 + 5] * m;
								const float side_left = buf[i * 4 + 6] * m;
								const float side_right = buf[i * 4 + 7] * m;

								const float mid = (center + low_freq) * 0.708f;
								buf2ch[i + 0] = (left + rear_left + side_left + mid) * k;
								buf2ch[i + 1] = (right + rear_right + side_right + mid) * k;

								buf8ch[i * 4 + 0] = left;
								buf8ch[i * 4 + 1] = right;
								buf8ch[i * 4 + 2] = center;
								buf8ch[i * 4 + 3] = low_freq;
								buf8ch[i * 4 + 4] = rear_left;
								buf8ch[i * 4 + 5] = rear_right;
								buf8ch[i * 4 + 6] = side_left;
								buf8ch[i * 4 + 7] = side_right;
							}
							first_mix = false;
						}
						else
						{
							for (u32 i = 0; i < (sizeof(buf2ch) / sizeof(float)); i += 2)
							{
								const float left = buf[i * 4 + 0] * m;
								const float right = buf[i * 4 + 1] * m;
								const float center = buf[i * 4 + 2] * m;
								const float low_freq = buf[i * 4 + 3] * m;
								const float rear_left = buf[i * 4 + 4] * m;
								const float rear_right = buf[i * 4 + 5] * m;
								const float side_left = buf[i * 4 + 6] * m;
								const float side_right = buf[i * 4 + 7] * m;

								const float mid = (center + low_freq) * 0.708f;
								buf2ch[i + 0] += (left + rear_left + side_left + mid) * k;
								buf2ch[i + 1] += (right + rear_right + side_right + mid) * k;

								buf8ch[i * 4 + 0] += left;
								buf8ch[i * 4 + 1] += right;
								buf8ch[i * 4 + 2] += center;
								buf8ch[i * 4 + 3] += low_freq;
								buf8ch[i * 4 + 4] += rear_left;
								buf8ch[i * 4 + 5] += rear_right;
								buf8ch[i * 4 + 6] += side_left;
								buf8ch[i * 4 + 7] += side_right;
							}
						}
					}

					memset(buf, 0, block_size * sizeof(float));
				}

				// convert the data from float to u16 with clipping:
				if (!first_mix)
				{
					// 2x MULPS
					// 2x MAXPS (optional)
					// 2x MINPS (optional)
					// 2x CVTPS2DQ (converts float to s32)
					// PACKSSDW (converts s32 to s16 with signed saturation)

					if (g_is_u16)
					{
						for (u32 i = 0; i < (sizeof(buf2ch) / sizeof(float)); i += 8)
						{
							static const __m128 float2u16 = { 0x8000, 0x8000, 0x8000, 0x8000 };
							(__m128i&)(oal_buffer[oal_pos][oal_buffer_offset + i]) = _mm_packs_epi32(
								_mm_cvtps_epi32(_mm_mul_ps((__m128&)(buf2ch[i]), float2u16)),
								_mm_cvtps_epi32(_mm_mul_ps((__m128&)(buf2ch[i + 4]), float2u16)));
						}
					}
					else
					for (u32 i = 0; i < (sizeof(buf2ch) / sizeof(float)); i++)
					{
						oal_buffer_float[oal_pos][oal_buffer_offset + i] = buf2ch[i];
					}
				}

				//const u64 stamp1 = get_system_time();

				if (first_mix)
				{
					if (g_is_u16) memset(&oal_buffer[oal_pos][0], 0, oal_buffer_size * sizeof(s16));
					else memset(&oal_buffer_float[oal_pos][0], 0, oal_buffer_size * sizeof(float));
				}
				oal_buffer_offset += sizeof(buf2ch) / sizeof(float);

				if(oal_buffer_offset >= oal_buffer_size)
				{
					if(m_audio_out)
					{
						if (g_is_u16)
							queue.Push(&oal_buffer[oal_pos][0], nullptr);

						queue_float.Push(&oal_buffer_float[oal_pos][0], nullptr);
					}

					oal_buffer_offset = 0;
				}

				//const u64 stamp2 = get_system_time();

				// send aftermix event (normal audio event)
				{
					std::lock_guard<std::mutex> lock(audioMutex);
					// update indexes:
					auto indexes = vm::ptr<u64>::make(m_config.m_indexes);
					for (u32 i = 0; i < m_config.AUDIO_PORT_COUNT; i++)
					{
						if (!m_config.m_ports[i].m_is_audio_port_started) continue;

						AudioPortConfig& port = m_config.m_ports[i];

						u32 position = port.tag % port.block; // old value
						port.counter = m_config.counter;
						port.tag++; // absolute index of block that will be read
						indexes[i] = (position + 1) % port.block; // write new value
					}
					// load keys:
					keys.resize(m_config.m_keys.size());
					memcpy(keys.data(), m_config.m_keys.data(), sizeof(u64) * keys.size());
				}
				for (u32 i = 0; i < keys.size(); i++)
				{
					// TODO: check event source
					Emu.GetEventManager().SendEvent(keys[i], 0x10103000e010e07, 0, 0, 0);
				}

				//const u64 stamp3 = get_system_time();

				if (do_dump && !first_mix)
				{
					if (m_dump.GetCh() == 8)
					{
						if (m_dump.WriteData(&buf8ch, sizeof(buf8ch)) != sizeof(buf8ch)) // write file data
						{
							cellAudio->Error("cellAudioInit(): AudioDumper::WriteData() failed");
							goto abort;
						}
					}
					else if (m_dump.GetCh() == 2)
					{
						if (m_dump.WriteData(&buf2ch, sizeof(buf2ch)) != sizeof(buf2ch)) // write file data
						{
							cellAudio->Error("cellAudioInit(): AudioDumper::WriteData() failed");
							goto abort;
						}
					}
					else
					{
						cellAudio->Error("cellAudioInit(): unknown AudioDumper::GetCh() value (%d)", m_dump.GetCh());
						goto abort;
					}
				}

				//LOG_NOTICE(HLE, "Audio perf: start=%d (access=%d, AddData=%d, events=%d, dump=%d)",
					//stamp0 - m_config.start_time, stamp1 - stamp0, stamp2 - stamp1, stamp3 - stamp2, get_system_time() - stamp3);
			}
			cellAudio->Notice("Audio thread ended");
abort:
			queue.Push(nullptr, nullptr);
			queue_float.Push(nullptr, nullptr);

			if(do_dump)
				m_dump.Finalize();

			m_config.m_is_audio_initialized = false;

			m_config.m_keys.clear();
			for (u32 i = 0; i < m_config.AUDIO_PORT_COUNT; i++)
			{
				AudioPortConfig& port = m_config.m_ports[i];
				port.m_is_audio_port_opened = false;
				port.m_is_audio_port_started = false;
			}
			m_config.m_port_in_use = 0;

			while (!internal_finished)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}

			m_config.m_is_audio_finalized = true;
		});
	t.detach();

	while (!m_config.start_time) // waiting for initialization
	{
		if (Emu.IsStopped())
		{
			cellAudio->Warning("cellAudioInit() aborted");
			return CELL_OK;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	return CELL_OK;
}

int cellAudioQuit()
{
	cellAudio->Warning("cellAudioQuit()");

	if (!m_config.m_is_audio_initialized)
	{
		return CELL_AUDIO_ERROR_NOT_INIT;
	}

	m_config.m_is_audio_initialized = false;

	while (!m_config.m_is_audio_finalized)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (Emu.IsStopped())
		{
			cellAudio->Warning("cellAudioQuit(): aborted");
			return CELL_OK;
		}
	}

	Memory.Free(m_config.m_buffer);
	Memory.Free(m_config.m_indexes);

	return CELL_OK;
}

int cellAudioPortOpen(vm::ptr<CellAudioPortParam> audioParam, vm::ptr<u32> portNum)
{
	cellAudio->Warning("cellAudioPortOpen(audioParam_addr=0x%x, portNum_addr=0x%x)", audioParam.addr(), portNum.addr());

	if (audioParam->nChannel > 8 || audioParam->nBlock > 16)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (m_config.m_port_in_use >= m_config.AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PORT_FULL;
	}

	for (u32 i = 0; i < m_config.AUDIO_PORT_COUNT; i++)
	{
		if (!m_config.m_ports[i].m_is_audio_port_opened)
		{
			AudioPortConfig& port = m_config.m_ports[i];
	
			port.channel = (u8)audioParam->nChannel;
			port.block = (u8)audioParam->nBlock;
			port.attr = audioParam->attr;
			port.addr = m_config.m_buffer + (128 * 1024 * i);
			port.read_index_addr = m_config.m_indexes + (sizeof(u64) * i);
			if (port.attr & CELL_AUDIO_PORTATTR_INITLEVEL)
			{
				port.level = audioParam->level;
			}
			else
			{
				port.level = 1.0f;
			}

			*portNum = i;
			cellAudio->Warning("*** audio port opened(nChannel=%d, nBlock=%d, attr=0x%llx, level=%f): port = %d",
				port.channel, port.block, port.attr, port.level, i);
			
			port.m_is_audio_port_opened = true;
			port.m_is_audio_port_started = false;
			port.tag = 0;

			m_config.m_port_in_use++;
			return CELL_OK;
		}
	}

	return CELL_AUDIO_ERROR_PORT_FULL;
}

int cellAudioGetPortConfig(u32 portNum, vm::ptr<CellAudioPortConfig> portConfig)
{
	cellAudio->Warning("cellAudioGetPortConfig(portNum=0x%x, portConfig_addr=0x%x)", portNum, portConfig.addr());

	if (portNum >= m_config.AUDIO_PORT_COUNT) 
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (!m_config.m_ports[portNum].m_is_audio_port_opened)
	{
		portConfig->status = CELL_AUDIO_STATUS_CLOSE;
	}
	else if (m_config.m_ports[portNum].m_is_audio_port_started)
	{
		portConfig->status = CELL_AUDIO_STATUS_RUN;
	}
	else
	{
		portConfig->status = CELL_AUDIO_STATUS_READY;
	}

	AudioPortConfig& port = m_config.m_ports[portNum];

	portConfig->nChannel = port.channel;
	portConfig->nBlock = port.block;
	portConfig->portSize = port.channel * port.block * 256 * sizeof(float);
	portConfig->portAddr = port.addr; // 0x20020000
	portConfig->readIndexAddr = port.read_index_addr; // 0x20010010 on ps3

	cellAudio->Log("*** port config: nChannel=%d, nBlock=%d, portSize=0x%x, portAddr=0x%x, readIndexAddr=0x%x",
		(u32)portConfig->nChannel, (u32)portConfig->nBlock, (u32)portConfig->portSize, (u32)portConfig->portAddr, (u32)portConfig->readIndexAddr);
	// portAddr - readIndexAddr ==  0xFFF0 on ps3

	return CELL_OK;
}

int cellAudioPortStart(u32 portNum)
{
	cellAudio->Warning("cellAudioPortStart(portNum=0x%x)", portNum);

	if (portNum >= m_config.AUDIO_PORT_COUNT) 
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (!m_config.m_ports[portNum].m_is_audio_port_opened)
	{
		return CELL_AUDIO_ERROR_PORT_OPEN;
	}

	if (m_config.m_ports[portNum].m_is_audio_port_started)
	{
		return CELL_AUDIO_ERROR_PORT_ALREADY_RUN;
	}
	
	m_config.m_ports[portNum].m_is_audio_port_started = true;

	return CELL_OK;
}

int cellAudioPortClose(u32 portNum)
{
	cellAudio->Warning("cellAudioPortClose(portNum=0x%x)", portNum);

	if (portNum >= m_config.AUDIO_PORT_COUNT) 
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (!m_config.m_ports[portNum].m_is_audio_port_opened)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	}

	m_config.m_ports[portNum].m_is_audio_port_started = false;
	m_config.m_ports[portNum].m_is_audio_port_opened = false;
	m_config.m_port_in_use--;
	return CELL_OK;
}

int cellAudioPortStop(u32 portNum)
{
	cellAudio->Warning("cellAudioPortStop(portNum=0x%x)",portNum);
	
	if (portNum >= m_config.AUDIO_PORT_COUNT) 
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (!m_config.m_ports[portNum].m_is_audio_port_opened)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	}

	if (!m_config.m_ports[portNum].m_is_audio_port_started)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_RUN;
	}
	
	m_config.m_ports[portNum].m_is_audio_port_started = false;
	return CELL_OK;
}

int cellAudioGetPortTimestamp(u32 portNum, u64 tag, vm::ptr<u64> stamp)
{
	cellAudio->Log("cellAudioGetPortTimestamp(portNum=0x%x, tag=0x%llx, stamp_addr=0x%x)", portNum, tag, stamp.addr());

	if (portNum >= m_config.AUDIO_PORT_COUNT) 
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (!m_config.m_ports[portNum].m_is_audio_port_opened)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	}

	if (!m_config.m_ports[portNum].m_is_audio_port_started)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_RUN;
	}

	AudioPortConfig& port = m_config.m_ports[portNum];

	std::lock_guard<std::mutex> lock(audioMutex);

	*stamp = m_config.start_time + (port.counter + (tag - port.tag)) * 256000000 / 48000;

	return CELL_OK;
}

int cellAudioGetPortBlockTag(u32 portNum, u64 blockNo, vm::ptr<u64> tag)
{
	cellAudio->Log("cellAudioGetPortBlockTag(portNum=0x%x, blockNo=0x%llx, tag_addr=0x%x)", portNum, blockNo, tag.addr());

	if (portNum >= m_config.AUDIO_PORT_COUNT) 
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (!m_config.m_ports[portNum].m_is_audio_port_opened)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	}

	if (!m_config.m_ports[portNum].m_is_audio_port_started)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_RUN;
	}

	AudioPortConfig& port = m_config.m_ports[portNum];

	if (blockNo >= port.block)
	{
		cellAudio->Error("cellAudioGetPortBlockTag: wrong blockNo(%lld)", blockNo);
		return CELL_AUDIO_ERROR_PARAM;
	}

	std::lock_guard<std::mutex> lock(audioMutex);

	u64 tag_base = port.tag;
	if (tag_base % port.block > blockNo)
	{
		tag_base &= ~(port.block-1);
		tag_base += port.block;
	}
	else
	{
		tag_base &= ~(port.block-1);
	}
	*tag = tag_base + blockNo;

	return CELL_OK;
}

int cellAudioSetPortLevel(u32 portNum, float level)
{
	cellAudio->Todo("cellAudioSetPortLevel(portNum=0x%x, level=%f)", portNum, level);

	AudioPortConfig& port = m_config.m_ports[portNum];

	if (portNum >= m_config.AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (!port.m_is_audio_port_opened)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	}

	if (!port.m_is_audio_port_started)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_RUN;
	}

	std::lock_guard<std::mutex> lock(audioMutex);

	port.level = level; // TODO

	return CELL_OK;
}

// Utility Functions  
int cellAudioCreateNotifyEventQueue(vm::ptr<u32> id, vm::ptr<u64> key)
{
	cellAudio->Warning("cellAudioCreateNotifyEventQueue(id_addr=0x%x, key_addr=0x%x)", id.addr(), key.addr());

	std::lock_guard<std::mutex> lock(audioMutex);

	u64 event_key = 0;
	while (Emu.GetEventManager().CheckKey((event_key << 48) | 0x80004d494f323221))
	{
		event_key++; // experimental
		//return CELL_AUDIO_ERROR_EVENT_QUEUE;
	}
	event_key = (event_key << 48) | 0x80004d494f323221; // left part: 0x8000, 0x8001, 0x8002 ...

	EventQueue* eq = new EventQueue(SYS_SYNC_FIFO, SYS_PPU_QUEUE, event_key, event_key, 32);

	if (!Emu.GetEventManager().RegisterKey(eq, event_key))
	{
		delete eq;
		return CELL_AUDIO_ERROR_EVENT_QUEUE;
	}

	*id = cellAudio->GetNewId(eq);
	*key = event_key;

	return CELL_OK;
}

int cellAudioCreateNotifyEventQueueEx(vm::ptr<u32> id, vm::ptr<u64> key, u32 iFlags)
{
	cellAudio->Todo("cellAudioCreateNotifyEventQueueEx(id_addr=0x%x, key_addr=0x%x, iFlags=0x%x)", id.addr(), key.addr(), iFlags);
	return CELL_OK;
}

int cellAudioSetNotifyEventQueue(u64 key)
{
	cellAudio->Warning("cellAudioSetNotifyEventQueue(key=0x%llx)", key);

	std::lock_guard<std::mutex> lock(audioMutex);

	for (u32 i = 0; i < m_config.m_keys.size(); i++) // check for duplicates
	{
		if (m_config.m_keys[i] == key)
		{
			return CELL_AUDIO_ERROR_PARAM;
		}
	}
	m_config.m_keys.push_back(key);

	/*EventQueue* eq;
	if (!Emu.GetEventManager().GetEventQueue(key, eq))
	{
		return CELL_AUDIO_ERROR_PARAM;
	}*/

	// TODO: connect port (?????)

	return CELL_OK;
}

int cellAudioSetNotifyEventQueueEx(u64 key, u32 iFlags)
{
	cellAudio->Todo("cellAudioSetNotifyEventQueueEx(key=0x%llx, iFlags=0x%x)", key, iFlags);
	return CELL_OK;
}

int cellAudioRemoveNotifyEventQueue(u64 key)
{
	cellAudio->Warning("cellAudioRemoveNotifyEventQueue(key=0x%llx)", key);

	std::lock_guard<std::mutex> lock(audioMutex);

	bool found = false;
	for (u32 i = 0; i < m_config.m_keys.size(); i++)
	{
		if (m_config.m_keys[i] == key)
		{
			m_config.m_keys.erase(m_config.m_keys.begin() + i);
			found = true;
			break;
		}
	}

	if (!found)
	{
		// ???
		return CELL_AUDIO_ERROR_PARAM;
	}

	/*EventQueue* eq;
	if (!Emu.GetEventManager().GetEventQueue(key, eq))
	{
		return CELL_AUDIO_ERROR_PARAM;
	}*/

	// TODO: disconnect port

	return CELL_OK;
}

int cellAudioRemoveNotifyEventQueueEx(u64 key, u32 iFlags)
{
	cellAudio->Todo("cellAudioRemoveNotifyEventQueueEx(key=0x%llx, iFlags=0x%x)", key, iFlags);
	return CELL_OK;
}

int cellAudioAddData(u32 portNum, vm::ptr<float> src, u32 samples, float volume)
{
	cellAudio->Todo("cellAudioAddData(portNum=0x%x, src_addr=0x%x, samples=%d, volume=%f)", portNum, src.addr(), samples, volume);
	
	AudioPortConfig& port = m_config.m_ports[portNum];

	if (portNum >= m_config.AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (!port.m_is_audio_port_opened)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	}

	if (!port.m_is_audio_port_started)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_RUN;
	}

	std::lock_guard<std::mutex> lock(audioMutex);
	
	//u32 addr = port.addr;
	//for (u32 i = 0; i < samples; i++)
	//{
	//	vm::write32(addr, src[i]);
	//	addr += port.channel * port.block * sizeof(float);
	//}

	m_config.m_buffer = src.addr(); // TODO: write data from src in selected port

	return CELL_OK;
}

int cellAudioAdd2chData(u32 portNum, vm::ptr<be_t<float>> src, u32 samples, float volume)
{
	cellAudio->Todo("cellAudioAdd2chData(portNum=0x%x, src_addr=0x%x, samples=%d, volume=%f)", portNum, src.addr(), samples, volume);
	
	AudioPortConfig& port = m_config.m_ports[portNum];

	if (portNum >= m_config.AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (!port.m_is_audio_port_opened)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	}

	if (!port.m_is_audio_port_started)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_RUN;
	}

	std::lock_guard<std::mutex> lock(audioMutex);

	m_config.m_buffer = src.addr(); // TODO
	
	return CELL_OK;
}

int cellAudioAdd6chData(u32 portNum, vm::ptr<be_t<float>> src, float volume)
{
	cellAudio->Todo("cellAudioAdd6chData(portNum=0x%x, src_addr=0x%x, volume=%f)", portNum, src.addr(), volume);
	
	AudioPortConfig& port = m_config.m_ports[portNum];

	if (portNum >= m_config.AUDIO_PORT_COUNT)
	{
		return CELL_AUDIO_ERROR_PARAM;
	}

	if (!port.m_is_audio_port_opened)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_OPEN;
	}

	if (!port.m_is_audio_port_started)
	{
		return CELL_AUDIO_ERROR_PORT_NOT_RUN;
	}

	std::lock_guard<std::mutex> lock(audioMutex);

	m_config.m_buffer = src.addr(); // TODO
	
	return CELL_OK;
}

int cellAudioMiscSetAccessoryVolume(u32 devNum, float volume)
{
	cellAudio->Todo("cellAudioMiscSetAccessoryVolume(devNum=0x%x, volume=%f)", devNum, volume);
	return CELL_OK;
}

int cellAudioSendAck(u64 data3)
{
	cellAudio->Todo("cellAudioSendAck(data3=0x%llx)", data3);
	return CELL_OK;
}

int cellAudioSetPersonalDevice(int iPersonalStream, int iDevice)
{
	cellAudio->Todo("cellAudioSetPersonalDevice(iPersonalStream=0x%x, iDevice=0x%x)", iPersonalStream, iDevice);
	return CELL_OK;
}

int cellAudioUnsetPersonalDevice(int iPersonalStream)
{
	cellAudio->Todo("cellAudioUnsetPersonalDevice(iPersonalStream=0x%x)", iPersonalStream);
	return CELL_OK;
}

void cellAudio_init(Module *pxThis)
{
	cellAudio = pxThis;

	cellAudio->AddFunc(0x0b168f92, cellAudioInit);
	cellAudio->AddFunc(0x4129fe2d, cellAudioPortClose);
	cellAudio->AddFunc(0x5b1e2c73, cellAudioPortStop);
	cellAudio->AddFunc(0x74a66af0, cellAudioGetPortConfig);
	cellAudio->AddFunc(0x89be28f2, cellAudioPortStart);
	cellAudio->AddFunc(0xca5ac370, cellAudioQuit);
	cellAudio->AddFunc(0xcd7bc431, cellAudioPortOpen);
	cellAudio->AddFunc(0x56dfe179, cellAudioSetPortLevel);
	cellAudio->AddFunc(0x04af134e, cellAudioCreateNotifyEventQueue);
	cellAudio->AddFunc(0x31211f6b, cellAudioMiscSetAccessoryVolume);
	cellAudio->AddFunc(0x377e0cd9, cellAudioSetNotifyEventQueue);
	cellAudio->AddFunc(0x4109d08c, cellAudioGetPortTimestamp);
	cellAudio->AddFunc(0x9e4b1db8, cellAudioAdd2chData);
	cellAudio->AddFunc(0xdab029aa, cellAudioAddData);
	cellAudio->AddFunc(0xe4046afe, cellAudioGetPortBlockTag);
	cellAudio->AddFunc(0xff3626fd, cellAudioRemoveNotifyEventQueue);
}
