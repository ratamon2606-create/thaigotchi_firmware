#!/usr/bin/env python3
"""
Thaigotchi Audio Generator
Generates all required WAV files for SD card

Requirements:
    pip install gtts
    + ffmpeg installed
        macOS: brew install ffmpeg
        Linux: apt install ffmpeg
        Windows: download from ffmpeg.org

Usage:
    python3 generate_audio.py
    
Output: sd_card_files/ folder with all .wav files
"""

import os
import subprocess
import sys

# Check dependencies
try:
    from gtts import gTTS
except ImportError:
    print("❌ gtts not installed. Run: pip install gtts")
    sys.exit(1)

# Check ffmpeg
try:
    subprocess.run(['ffmpeg', '-version'], 
                   capture_output=True, check=True)
except (FileNotFoundError, subprocess.CalledProcessError):
    print("❌ ffmpeg not found. Install:")
    print("   macOS:   brew install ffmpeg")
    print("   Linux:   sudo apt install ffmpeg")
    print("   Windows: https://ffmpeg.org/download.html")
    sys.exit(1)

# Output directory
OUTPUT_DIR = "sd_card_files"
os.makedirs(OUTPUT_DIR, exist_ok=True)


# ============================================
# AUDIO SCRIPTS — Edit text here if you want!
# ============================================
AUDIO_SCRIPTS = {
    # ─── BOOT ───
    "welcome": (
        "สวัสดีครับ ผมมฤคนทีนะครับ "
        "ผมมีเรื่องเล่าให้ฟังเยอะเลย "
        "อยากเลี้ยงผมมั้ยครับ "
        "เปิดแอปแล้วเชื่อมต่อกันเลยครับ"
    ),
    
    # ─── THINKING (filler ระหว่าง RAG) ───
    "thinking_01": "เอ่อ...",
    "thinking_02": "อืม ขอคิดดูก่อนนะครับ",
    "thinking_03": "เดี๋ยวนะครับ",
    
    # ─── ACK (กดปุ่ม) ───
    "listening": "ฮะ ครับ",
    "got_it": "เข้าใจแล้วครับ",
    
    # ─── LOCATION UNLOCKS ───
    "wat_phra_kaew_intro": (
        "ยินดีต้อนรับสู่วัดพระแก้วนะครับ "
        "วัดนี้สร้างขึ้นในปี พ.ศ. 2325 "
        "พร้อมกับการสถาปนากรุงรัตนโกสินทร์ "
        "โดยพระบาทสมเด็จพระพุทธยอดฟ้าจุฬาโลกมหาราช "
        "เป็นวัดประจำพระบรมมหาราชวัง "
        "ที่ประดิษฐานพระแก้วมรกต "
        "พระพุทธรูปคู่บ้านคู่เมืองของไทย "
        "แกะสลักจากหยกสีเขียวเข้ม "
        "สูงประมาณ 66 เซนติเมตร "
        "ค้นพบครั้งแรกที่จังหวัดเชียงราย "
        "ในปี พ.ศ. 1977 "
        "พระมหากษัตริย์จะทรงเปลี่ยนเครื่องทรง "
        "ตามฤดูกาล 3 ฤดู คือ "
        "ฤดูร้อน ฤดูฝน และฤดูหนาวครับ"
    ),
    
    "wat_arun_intro": (
        "ยินดีต้อนรับสู่วัดอรุณราชวรารามครับ "
        "หรือที่เรียกกันว่าวัดแจ้ง "
        "เป็นวัดโบราณตั้งแต่สมัยอยุธยา "
        "ในรัชสมัยพระเจ้าตากสินมหาราช "
        "เคยเป็นที่ประดิษฐานพระแก้วมรกต "
        "ก่อนที่จะอัญเชิญไปวัดพระแก้ว "
        "ชื่ออรุณหมายถึงรุ่งอรุณ "
        "เพราะกองทัพของพระเจ้าตากสิน "
        "มาถึงที่นี่เมื่อรุ่งสาง "
        "พระปรางค์วัดอรุณสูงประมาณ 82 เมตร "
        "เป็นสัญลักษณ์ของเขาพระสุเมรุ "
        "ในจักรวาลพุทธ "
        "ตกแต่งด้วยเศษเครื่องเคลือบจีน "
        "หลากสีนับล้านชิ้นครับ"
    ),
    
    "wat_pho_intro": (
        "ยินดีต้อนรับสู่วัดโพธิ์ครับ "
        "หรือวัดพระเชตุพนวิมลมังคลาราม "
        "เป็นวัดเก่าแก่ที่สุดในกรุงเทพมหานคร "
        "มีมาตั้งแต่สมัยอยุธยา "
        "รัชกาลที่ 1 ทรงปฏิสังขรณ์ครั้งใหญ่ "
        "ทำให้กลายเป็นวัดสำคัญ "
        "ในยุครัตนโกสินทร์ตอนต้น "
        "ที่นี่มีพระพุทธไสยาส "
        "พระพุทธรูปขนาดใหญ่ที่สุดในกรุงเทพ "
        "ยาว 46 เมตร สูง 15 เมตร "
        "สร้างในรัชสมัยรัชกาลที่ 3 "
        "ที่ฝ่าพระบาทประดับมุก "
        "เป็นรูปมงคล 108 ประการ "
        "และวัดโพธิ์ยังเป็นต้นกำเนิด "
        "ของการนวดแผนไทยอีกด้วยครับ"
    ),
}


def text_to_wav(text, name, slow=False):
    """Generate WAV via Google TTS → ffmpeg."""
    mp3_path = os.path.join(OUTPUT_DIR, f"{name}.mp3")
    wav_path = os.path.join(OUTPUT_DIR, f"{name}.wav")
    
    print(f"  🎤 {name}...", end=" ", flush=True)
    
    # Generate MP3
    tts = gTTS(text=text, lang='th', slow=slow)
    tts.save(mp3_path)
    
    # Convert to WAV 16kHz mono PCM 16-bit
    subprocess.run([
        "ffmpeg", "-y", "-loglevel", "error",
        "-i", mp3_path,
        "-ar", "16000",
        "-ac", "1",
        "-c:a", "pcm_s16le",
        wav_path
    ], check=True)
    
    os.remove(mp3_path)
    
    size_kb = os.path.getsize(wav_path) / 1024
    print(f"({size_kb:.1f} KB) ✓")


def main():
    print(f"🎤 Thaigotchi Audio Generator")
    print(f"📂 Output: ./{OUTPUT_DIR}/\n")
    print(f"Generating {len(AUDIO_SCRIPTS)} audio files...\n")
    
    success = 0
    for name, text in AUDIO_SCRIPTS.items():
        try:
            text_to_wav(text, name)
            success += 1
        except Exception as e:
            print(f"❌ {e}")
    
    files = sorted(
        f for f in os.listdir(OUTPUT_DIR) if f.endswith('.wav')
    )
    total_mb = sum(
        os.path.getsize(os.path.join(OUTPUT_DIR, f))
        for f in files
    ) / (1024 * 1024)
    
    print(f"\n{'='*50}")
    print(f"✅ Generated {success}/{len(AUDIO_SCRIPTS)} files")
    print(f"💾 Total: {total_mb:.2f} MB")
    print(f"{'='*50}")
    print(f"\nNext steps:")
    print(f"1. Insert SD card in computer (FAT32 format)")
    print(f"2. Copy all .wav files from {OUTPUT_DIR}/ to SD root")
    print(f"3. Eject SD card → insert into ESP32 module")
    print(f"4. ESP32 plays via: ble.playAudio('welcome')")


if __name__ == "__main__":
    main()
