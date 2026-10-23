package com.vi.phoneclient

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import java.io.ByteArrayOutputStream

/**
 * PTT 录音器：16kHz 单声道 PCM16 → WAV 字节。
 * 语义对齐 voice_input 的录音参数（服务端 ASR 按 16k WAV 接收）。
 */
class PttRecorder {

    private var record: AudioRecord? = null
    private var thread: Thread? = null
    @Volatile private var running = false
    private val pcm = ByteArrayOutputStream()

    val isRecording: Boolean get() = running

    @SuppressLint("MissingPermission")
    fun start(): Boolean {
        if (running) return true
        val sampleRate = 16000
        val minBuf = AudioRecord.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT
        )
        val rec = try {
            AudioRecord(
                MediaRecorder.AudioSource.VOICE_RECOGNITION,
                sampleRate,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                maxOf(minBuf, 8192)
            )
        } catch (e: Exception) {
            return false
        }
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            rec.release()
            return false
        }
        pcm.reset()
        record = rec
        running = true
        rec.startRecording()
        thread = Thread {
            val buf = ByteArray(3200)  // 100ms @16k mono 16bit
            while (running) {
                val n = rec.read(buf, 0, buf.size)
                if (n > 0) {
                    synchronized(pcm) { pcm.write(buf, 0, n) }
                }
            }
        }.also { it.start() }
        return true
    }

    /** 停止录音并返回 WAV 字节；无数据返回空数组。 */
    fun stop(): ByteArray {
        if (!running) return ByteArray(0)
        running = false
        thread?.join(1000)
        thread = null
        record?.let {
            try {
                it.stop()
            } catch (_: Exception) {
            }
            it.release()
        }
        record = null
        val data = synchronized(pcm) { pcm.toByteArray() }
        pcm.reset()
        if (data.isEmpty()) return ByteArray(0)
        return wavWrap(data, 16000)
    }

    fun cancel() {
        running = false
        thread?.join(1000)
        thread = null
        record?.let {
            try {
                it.stop()
            } catch (_: Exception) {
            }
            it.release()
        }
        record = null
        pcm.reset()
    }

    private fun wavWrap(pcmData: ByteArray, sampleRate: Int): ByteArray {
        val out = ByteArrayOutputStream(pcmData.size + 44)
        fun int32(v: Int) {
            out.write(v and 0xFF); out.write((v shr 8) and 0xFF)
            out.write((v shr 16) and 0xFF); out.write((v shr 24) and 0xFF)
        }
        fun int16(v: Int) {
            out.write(v and 0xFF); out.write((v shr 8) and 0xFF)
        }
        fun str(s: String) = out.write(s.toByteArray(Charsets.US_ASCII))

        val dataLen = pcmData.size
        str("RIFF"); int32(36 + dataLen); str("WAVE")
        str("fmt "); int32(16)
        int16(1)              // PCM
        int16(1)              // mono
        int32(sampleRate)
        int32(sampleRate * 2) // byte rate
        int16(2)              // block align
        int16(16)             // bits
        str("data"); int32(dataLen)
        out.write(pcmData)
        return out.toByteArray()
    }
}
