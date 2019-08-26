#define NOMINMAX

// FFmpeg���ۂ��\���ŏo�͂��郂�[�h�i�f�o�b�O�p�j
//#define LIKE_FFMPEG 1

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "MeiToAVI.hpp"
#include "ApproxFraction.hpp"
#include "AVI.hpp"
#include "Fraction.hpp"
#include "RIFF/RIFFChunk.hpp"
#include "RIFF/RIFFList.hpp"
#include "RIFF/RIFFRoot.hpp"
#include "Source/CachedSource.hpp"
#include "Source/MemorySource.hpp"

#include <Windows.h>

#include <sakuraglx/sakuraglx.h>
#include <sakuragl/sgl_erisa_lib.h>

using namespace std::literals;


namespace {
  constexpr std::uint_fast32_t MaxRiffSizeAVI  = 0x40000000;    // 1 GiB
  constexpr std::uint_fast32_t MaxRiffSizeAVIX = 0x40000000;    // 1 GiB

  constexpr std::uint_fast32_t MaxStreamsAVI   = 0xFFFFFFFF;
  constexpr std::uint_fast32_t MaxStreamsAVIX  = 0xFFFFFFFF;


  void CheckError(SSystem::SError error, const std::string& message) {
    if (error != SSystem::SError::errSuccess) {
      throw std::runtime_error(message);
    }
  }


  template<typename T>
  Fraction<T> ReduceFraction(const Fraction<T>& fraction) {
    const T gcd = std::gcd<T>(fraction.numerator, fraction.denominator);
    return Fraction<T>{
      fraction.numerator / gcd,
      fraction.denominator / gcd,
    };
  }


  constexpr std::uint32_t GetFourCC(const char fourcc[5]) {
    return
      static_cast<std::uint32_t>(fourcc[0]) <<  0 |
      static_cast<std::uint32_t>(fourcc[1]) <<  8 |
      static_cast<std::uint32_t>(fourcc[2]) << 16 |
      static_cast<std::uint32_t>(fourcc[3]) << 24;
  }


  template<typename T>
  std::shared_ptr<RIFFChunk> AddChunk(RIFFList& parent, std::uint32_t chunkId, const T& data) {
    auto memorySource = std::make_shared<MemorySource>(reinterpret_cast<const std::uint8_t*>(&data), sizeof(T));
    auto riffChunk = std::make_shared<RIFFChunk>(&parent, chunkId, memorySource);
    parent.AddChild(riffChunk);
    return riffChunk;
  }

  std::shared_ptr<RIFFChunk> AddEmptyChunk(RIFFList& parent, std::uint32_t chunkId) {
    auto riffChunk = std::make_shared<RIFFChunk>(&parent, chunkId);
    parent.AddChild(riffChunk);
    return riffChunk;
  }
}


MEIToAVI::FrameImageSource::FrameImageSource(ERISA::SGLMovieFilePlayer& movieFilePlayer, std::uint_fast32_t frameIndex) :
  mPtrMovieFilePlayer(&movieFilePlayer),
  mFrameIndex(frameIndex),
  mSize(0)
{
  const auto size = mPtrMovieFilePlayer->CurrentFrame()->GetImageSize();
  mSize = size.w * size.h * 4;
}


std::streamsize MEIToAVI::FrameImageSource::GetSize() const {
  return mSize;
}


void MEIToAVI::FrameImageSource::Read(std::uint8_t* data, std::size_t size, std::streamsize offset) {
  mPtrMovieFilePlayer->SeekToFrame(mFrameIndex);
  const auto ptrCurrentFrame = mPtrMovieFilePlayer->CurrentFrame();
  const auto ptrSmartImage = static_cast<SakuraGL::SGLSmartImage*>(ptrCurrentFrame);
  const auto ptrImageBuffer = ptrSmartImage->GetImage();
  std::memcpy(data, ptrImageBuffer->ptrBuffer + offset, size);
}



MEIToAVI::MEIToAVI(const std::wstring& filePath, const Options& options) :
  mCacheStorage(options.cacheStorageSize, options.cacheStorageLimit),
  mFile(),
  mMovieFilePlayer(),
  mRiffRoot()
{
  // open file
  CheckError(mFile.Open(filePath.c_str(), SSystem::SFileOpener::OpenFlag::modeRead | SSystem::SFileOpener::OpenFlag::shareRead), "cannot open file"s);

  // open as video
  CheckError(mMovieFilePlayer.OpenMovieFile(&mFile, false), "cannot open video"s);

  // get media
  const auto& mediaFile = mMovieFilePlayer.GetMediaFile();

  
  // check if the file has audio data
  bool hasAudio = false;
  if (!(options.flags & NoAudio)) {
    hasAudio = mediaFile.m_flagsRead & ERISA::SGLMediaFile::readSoundInfo;
  }


  // load audio
  std::vector<std::uint8_t> audioData;
  unsigned int audioBitsPerSample = 0;
  unsigned int audioNumChannels = 0;
  unsigned int audioBlockSize = 0;
  std::uint_fast32_t audioSamplingRate = 0;
  std::uint_fast32_t audioNumSamples = 0;

  if (hasAudio) {
    SSystem::SFile fileForSound;
    CheckError(fileForSound.Open(filePath.c_str(), SSystem::SFileOpener::OpenFlag::modeRead | SSystem::SFileOpener::OpenFlag::shareRead), "cannot open file for audio"s);

    ERISA::SGLSoundFilePlayer soundFilePlayer;
    CheckError(soundFilePlayer.OpenSoundFile(&fileForSound, false), "cannot open audio"s);

    audioBitsPerSample = soundFilePlayer.GetBitsPerSample();
    audioNumChannels = soundFilePlayer.GetChannelCount();
    audioSamplingRate = soundFilePlayer.GetFrequency();
    audioNumSamples = soundFilePlayer.GetTotalSampleCount();
    audioBlockSize = audioBitsPerSample / 8 * audioNumChannels;

    // �����f�[�^��S�ēǂݏo��
    // �\�ߓǂ�ł���͎̂��O�Ƀ`�����N�̔z�u�����肵�Ă����K�v�����邽��
    // mei�t�@�C���ł̔z�u�����̂܂�AVI�ɂ���̂��l����������͂���Ŗʓ|����������

    audioData.resize(audioNumSamples * audioBlockSize);

    SSystem::SArray<std::uint8_t> audioBuffer;
    std::uint_fast32_t offset = 0;
    while (offset < audioData.size()) {
      soundFilePlayer.GetNextWaveBuffer(audioBuffer);
      std::memcpy(audioData.data() + offset, audioBuffer.GetArray(), audioBuffer.GetLength());
      offset += audioBuffer.GetLength();

#ifdef _DEBUG
      OutputDebugStringW((L"Read audio: "s + std::to_wstring(offset) + L" / "s + std::to_wstring(audioData.size()) + L"\n"s).c_str());
#endif
    }

    soundFilePlayer.Close();
    fileForSound.Close();
  }


  // load video
  const bool videoHasAlpha = !(options.flags & NoAlpha) && mediaFile.m_eriInfoHeader.fdwFormatType == 0x04000001 /*ERI_RGBA_IMAGE*/;
  const auto videoSize = mMovieFilePlayer.CurrentFrame()->GetImageSize();
  const auto videoBytesPerFrame = static_cast<std::uint_fast32_t>(videoSize.w * videoSize.h * 4);
  const auto videoNumFrames = static_cast<std::uint_fast32_t>(mMovieFilePlayer.GetAllFrameCount());
  const auto videoDurationMillis = static_cast<std::uint_fast32_t>(mMovieFilePlayer.GetTotalTime());

  Fraction<std::uint_fast32_t> videoFPS{
    videoNumFrames * static_cast<std::uint_fast32_t>(1000),
    videoDurationMillis,
  };
  if (!(options.flags & NoApproxFPS)) {
    const auto orgFPS = videoFPS;

    videoFPS = ApproxFraction(videoFPS);

    if (!(options.flags & NoMessage)) {
      std::wcerr << L"[info] fps is approximated as "sv
                 << videoFPS.numerator << L"/"sv << videoFPS.denominator << L" ("sv << (static_cast<double>(videoFPS.numerator) / videoFPS.denominator) << L") from "sv
                 << orgFPS.numerator << L"/"sv << orgFPS.denominator << L" ("sv << (static_cast<double>(orgFPS.numerator) / orgFPS.denominator) << L")"sv <<std::endl;
    }
  }
  videoFPS = ReduceFraction(videoFPS);


  // check frame size
  if (!(options.flags & NoMessage)) {
    if (videoSize.w != mediaFile.m_eriInfoHeader.nImageWidth || videoSize.h != mediaFile.m_eriInfoHeader.nImageHeight) {
      std::wcerr << L"[warn] the resolution of the output video ("sv << videoSize.w << L"x"sv << videoSize.h
                 << L") will be different from the original ("sv << mediaFile.m_eriInfoHeader.nImageWidth << L"x"sv << mediaFile.m_eriInfoHeader.nImageHeight << L")"sv << std::endl;
    }
  }


  // 1�t���[��������̃T���v����
  // �S�ẴI�[�f�B�I�u���b�N�͂��̒P�ʂɂ���
  // ���ꂽ�ꍇ�̓u���b�N���̃T���v�����͕ς����Ƀu���b�N�̈ʒu�𒲐����č��킹��
  std::uint_fast32_t audioSamplesPerFrame = 0;

  if (hasAudio) {
    audioSamplesPerFrame = options.audioBlockSamples ? options.audioBlockSamples : audioSamplingRate* videoFPS.denominator / videoFPS.numerator;

    // 2�̔{���ɂ���
    audioSamplesPerFrame = (audioSamplesPerFrame + 1) & ~static_cast<decltype(audioSamplesPerFrame)>(1);

    // RIFF�̓`�����N��2�o�C�g�P�ʂŐ��񂷂�K�v������̂łƂ肠����
    // audioBlockSize����������Ζ��Ȃ����Ƃ����邪�ꉞ

#ifdef LIKE_FFMPEG
    audioSamplesPerFrame = 1024;
#endif
  }


  // construct RIFF

  // ## RIFF-AVI
  
  auto riffAvi = std::make_shared<RIFFList>(&mRiffRoot, GetFourCC("RIFF"), GetFourCC("AVI "));
  mRiffRoot.AddChild(riffAvi);

  // ### LIST-hdrl
  auto listHdrl = std::make_shared<RIFFList>(riffAvi.get(), GetFourCC("LIST"), GetFourCC("hdrl"));
  riffAvi->AddChild(listHdrl);

  // #### avih
  AVI::MainAVIHeader avihData{
    static_cast<std::uint32_t>(1.e6 * videoFPS.denominator / videoFPS.numerator),
    static_cast<std::uint32_t>(static_cast<std::uint_fast64_t>(videoBytesPerFrame) * videoFPS.numerator / videoFPS.denominator + static_cast<std::uint_fast64_t>(audioSamplingRate) * audioBlockSize),
    0u,
    AVI::AVIF_HASINDEX | AVI::AVIF_ISINTERLEAVED | AVI::AVIF_TRUSTCKTYPE,
    0u,   // filled later
    0u,
    hasAudio ? 2u : 1u,
    videoBytesPerFrame + 2 * 4,
    static_cast<std::uint32_t>(videoSize.w),
    static_cast<std::uint32_t>(videoSize.h),
  };
  auto avihMemorySource = std::make_shared<MemorySource>(reinterpret_cast<const std::uint8_t*>(&avihData), sizeof(avihData));
  auto avih = std::make_shared<RIFFChunk>(listHdrl.get(), GetFourCC("avih"), avihMemorySource);
  listHdrl->AddChild(avih);

  // video

  // #### LIST-strl (video)
  auto videoListStrl = std::make_shared<RIFFList>(listHdrl.get(), GetFourCC("LIST"), GetFourCC("strl"));
  listHdrl->AddChild(videoListStrl);

  // ##### strh (video)
  AddChunk(*videoListStrl, GetFourCC("strh"), AVI::AVIStreamHeader{
    GetFourCC("vids"),
    videoHasAlpha ? GetFourCC("RGBA") : GetFourCC("\0\0\0\0"),
    0u,
    0u,
    0u,
    0u,
    videoFPS.denominator,
    videoFPS.numerator,
    0u,
    videoNumFrames,
    videoBytesPerFrame,
    0xFFFFFFFFu,
    0u,
    {
      0u,
      0u,
      static_cast<std::uint16_t>(videoSize.w),
      static_cast<std::uint16_t>(videoSize.h),
    },
  });

  // ##### strf (video)
  AddChunk(*videoListStrl, GetFourCC("strf"), AVI::BITMAPINFOHEADER{
    sizeof(AVI::BITMAPINFOHEADER),
    static_cast<std::uint32_t>(videoSize.w),
    static_cast<std::uint32_t>(-videoSize.h),
    1u,
    32u,
    0u,   // BI_RGB
    static_cast<std::uint32_t>(videoSize.w * videoSize.h * 4),
    0u,
    0u,
    0u,
    0u,
  });

  // ##### indx (video)
  // set later
  auto videoIndx = std::make_shared<RIFFChunk>(videoListStrl.get(), GetFourCC("indx"));
  videoListStrl->AddChild(videoIndx);

  // audio
  
  std::shared_ptr<RIFFList> audioListStrl;
  std::shared_ptr<RIFFChunk> audioIndx;

  if (hasAudio) {
    // #### LIST-strl (audio)
    audioListStrl = std::make_shared<RIFFList>(listHdrl.get(), GetFourCC("LIST"), GetFourCC("strl"));
    listHdrl->AddChild(audioListStrl);

    // ##### strh (audio)
    AddChunk(*audioListStrl, GetFourCC("strh"), AVI::AVIStreamHeader{
      GetFourCC("auds"),
      GetFourCC("\1\0\0\0"),
      0u,
      0u,
      0u,
      0u,
      1u,
      audioSamplingRate,
      0u,
      audioNumSamples,
      audioSamplesPerFrame * audioBlockSize,
      0xFFFFFFFFu,
      audioBlockSize,
      {
        0u,
        0u,
        0u,
        0u,
      },
      });

    // ##### strf (audio)
    AddChunk(*audioListStrl, GetFourCC("strf"), AVI::WAVEFORMATEX{
      0x0001u,    // WAVE_FORMAT_PCM
      static_cast<std::uint16_t>(audioNumChannels),
      audioSamplingRate,
      audioSamplingRate * audioBlockSize,
      static_cast<std::uint16_t>(audioBlockSize),
      static_cast<std::uint16_t>(audioBitsPerSample),
    });

    // ##### indx (audio)
    audioIndx = std::make_shared<RIFFChunk>(audioListStrl.get(), GetFourCC("indx"));
    audioListStrl->AddChild(audioIndx);
  }
  
  // Open-DML

  // #### LIST-odml
  auto listOdml = std::make_shared<RIFFList>(listHdrl.get(), GetFourCC("LIST"), GetFourCC("odml"));
  listHdrl->AddChild(listOdml);

  // ##### dmlh
  AVI::AVIEXTHEADER dmlhData{
    static_cast<std::uint32_t>(videoNumFrames),
    {},
  };
  auto dmlhMemorySource = std::make_shared<MemorySource>(reinterpret_cast<const std::uint8_t*>(&dmlhData), sizeof(dmlhData));
  auto dmlh = std::make_shared<RIFFChunk>(listOdml.get(), GetFourCC("dmlh"), dmlhMemorySource);
  listOdml->AddChild(dmlh);

  //

  // ### LIST-INFO
  auto listInfo = std::make_shared<RIFFList>(riffAvi.get(), GetFourCC("LIST"), GetFourCC("INFO"));
  riffAvi->AddChild(listInfo);

  // #### ISFT
#if LIKE_FFMPEG
  const char isftStr[] = "MeiToAvi0.1.0";
  auto isftMemorySource = std::make_shared<MemorySource>(reinterpret_cast<const std::uint8_t*>(isftStr), 14);
#else
  const char isftStr[] = "MeiToAvi v0.1.0";
  static_assert(sizeof(isftStr) == 16);
  auto isftMemorySource = std::make_shared<MemorySource>(reinterpret_cast<const std::uint8_t*>(isftStr), sizeof(isftStr));
#endif
  auto isft = std::make_shared<RIFFChunk>(listInfo.get(), GetFourCC("ISFT"), isftMemorySource);
  listInfo->AddChild(isft);

#if LIKE_FFMPEG
  // ### JUNK (FFmpeg�̂ƍ��킹��p)
  auto junk = std::make_shared<RIFFChunk>(riffAvi.get(), GetFourCC("JUNK"), std::make_shared<MemorySource>(1016));
  riffAvi->AddChild(junk);
#else
  if (options.junkChunkSize) {
    auto junk = std::make_shared<RIFFChunk>(riffAvi.get(), GetFourCC("JUNK"), std::make_shared<MemorySource>(options.junkChunkSize));
    riffAvi->AddChild(junk);
  }
#endif

  // ### LIST-movi
  auto listMovi = std::make_shared<RIFFList>(riffAvi.get(), GetFourCC("LIST"), GetFourCC("movi"));
  riffAvi->AddChild(listMovi);

  // #### idx1
  auto idx1 = std::make_shared<RIFFChunk>(riffAvi.get(), GetFourCC("idx1"));
  riffAvi->AddChild(idx1);


  // ## RIFF-AVIX

  std::vector<std::pair<MemorySource*, RIFFList*>> standardIndices;   // ���qwBaseOffset���C������p

  struct StandardChunkInfo {
    std::shared_ptr<RIFFChunk> chunk;
    std::uint_fast32_t duration;
  };

  std::vector<StandardChunkInfo> videoStandardIndexChunks;
  std::vector<StandardChunkInfo> audioStandardIndexChunks;

  std::uint_fast32_t audioSampleCount = 0;
  std::uint_fast32_t videoFrameCount = 0;

  //

  constexpr std::uint_fast32_t InitialSizeCount = 8;

  std::shared_ptr<RIFFList> riffAvix = riffAvi;
  std::shared_ptr<RIFFList> avixListMovi = listMovi;

  std::vector<std::shared_ptr<RIFFChunk>> videoDataChunks;    // indx�o�^�p
  std::vector<std::shared_ptr<RIFFChunk>> audioDataChunks;    // indx�o�^�p

  struct ChunkInfo {
    bool video;
    std::shared_ptr<RIFFChunk> chunk;
  };
  std::vector<ChunkInfo> chunks;

  std::uint_fast32_t streamCount = 0;
  std::uint_fast32_t sizeCount = static_cast<std::uint_fast32_t>(riffAvix->GetSize());

  std::uint_fast32_t videoDurationCount = 0;
  std::uint_fast32_t audioDurationCount = 0;

  std::uint_fast64_t tempGcd = hasAudio ? std::gcd(audioSamplingRate, videoFPS.numerator) : 1;
  std::uint_fast64_t timeCodeCoefAudio = hasAudio ? videoFPS.numerator / tempGcd : 1;
  std::uint_fast64_t timeCodeCoefVideo = hasAudio ? static_cast<std::uint_fast64_t>(videoFPS.denominator) * (audioSamplingRate / tempGcd) : 1;

  while (true) {
    const bool isAvix = riffAvix != riffAvi;
    const bool finished = videoFrameCount == videoNumFrames && (!hasAudio || audioSampleCount == audioNumSamples);
    bool startNextAvix = false;

    const auto maxStreams = isAvix ? MaxStreamsAVIX : MaxStreamsAVI;
    const auto maxRiffSize = isAvix ? MaxRiffSizeAVIX : MaxRiffSizeAVI;

    const std::uint_fast64_t timeCodeVideo = videoFrameCount != videoNumFrames
      ? videoFrameCount * timeCodeCoefVideo
      : std::numeric_limits<std::uint_fast64_t>::max();

    const std::uint_fast64_t timeCodeAudio = hasAudio && audioSampleCount != audioNumSamples
      ? audioSampleCount * timeCodeCoefAudio
      : std::numeric_limits<std::uint_fast64_t>::max();

    // �Ȃ�ׂ��f���̕����ɒu���悤�ɂ���
    const bool isNextChunkVideo = timeCodeVideo <= timeCodeAudio;

    // ���̃`�����N�̑傫��
    const std::uint_fast32_t nextChunkSize = 8 + (isNextChunkVideo ? videoBytesPerFrame : std::min(audioSamplesPerFrame, audioNumSamples - audioSampleCount) * audioBlockSize);

    if (streamCount >= maxStreams || sizeCount + nextChunkSize >= maxRiffSize || finished) {
      // finish this RIFF-AVIX (RIFF-AVI) list

      // AVI-RIFF���X�g���ォ��LIST-movi���X�g���O�̗̈�̑傫�����܂���������\��������̂ŁA
      // AVI-RIFF���X�g����ɂ���ꍇ�͍Ō�ɃC���f�b�N�X�S�̂̃I�t�Z�b�g�����������K�v��������
      // �����ł�LIST-movi���X�g����ɂ��邱�Ƃł��̖�������Ă���
      // ���R����LIST-movi���X�g���̍\���ɕω�������ꍇ�͏�q�̖�肪������i�����ł͑��v�j

      auto& baseRiff = *avixListMovi;

      // video
      {
        const std::size_t videoIndexSize = sizeof(AVI::AVISTDINDEX) + sizeof(AVI::AVISTDINDEXENTRY) * videoDataChunks.size();
        auto videoIndexData = std::make_unique<std::uint8_t[]>(videoIndexSize);
        *reinterpret_cast<AVI::AVISTDINDEX*>(videoIndexData.get()) = AVI::AVISTDINDEX{
          2u,
          0u,
          1u,   // AVI_INDEX_OF_CHUNKS
          videoDataChunks.size(),
          GetFourCC("00dc"),
          0u,   // filled later
          0u,
        };
        const auto videoIndexEntries = reinterpret_cast<AVI::AVISTDINDEXENTRY*>(videoIndexData.get() + sizeof(AVI::AVISTDINDEX));
        const auto videoBaseOffset = baseRiff.GetOffset();
        for (std::uint_fast32_t i = 0; i < videoDataChunks.size(); i++) {
          auto& videoDataChunk = *videoDataChunks[i];
          videoIndexEntries[i] = AVI::AVISTDINDEXENTRY{
            static_cast<std::uint32_t>(videoDataChunk.GetOffset() - videoBaseOffset + 8),
            static_cast<std::uint32_t>(videoDataChunk.GetSize() - 8),
          };
        }
        auto videoIndexMemorySource = std::make_shared<MemorySource>(std::move(videoIndexData), videoIndexSize);
        auto videoIndexChunk = std::make_shared<RIFFChunk>(avixListMovi.get(), GetFourCC("ix00"), videoIndexMemorySource);
        avixListMovi->AddChild(videoIndexChunk);
        videoStandardIndexChunks.push_back(StandardChunkInfo{
          videoIndexChunk,
          videoDurationCount,
        });
        standardIndices.push_back(std::make_pair(videoIndexMemorySource.get(), &baseRiff));
      }

      // audio
      if (hasAudio) {
        const std::size_t audioIndexSize = sizeof(AVI::AVISTDINDEX) + sizeof(AVI::AVISTDINDEXENTRY) * audioDataChunks.size();
        auto audioIndexData = std::make_unique<std::uint8_t[]>(audioIndexSize);
        *reinterpret_cast<AVI::AVISTDINDEX*>(audioIndexData.get()) = AVI::AVISTDINDEX{
          2u,
          0u,
          1u,   // AVI_INDEX_OF_CHUNKS
          audioDataChunks.size(),
          GetFourCC("01wb"),
          0u,   // filled later
          0u,
        };
        const auto audioIndexEntries = reinterpret_cast<AVI::AVISTDINDEXENTRY*>(audioIndexData.get() + sizeof(AVI::AVISTDINDEX));
        const auto audioBaseOffset = baseRiff.GetOffset();
        for (std::uint_fast32_t i = 0; i < audioDataChunks.size(); i++) {
          auto& audioDataChunk = *audioDataChunks[i];
          audioIndexEntries[i] = AVI::AVISTDINDEXENTRY{
            static_cast<std::uint32_t>(audioDataChunk.GetOffset() - audioBaseOffset + 8),
            static_cast<std::uint32_t>(audioDataChunk.GetSize() - 8),
          };
        }
        auto audioIndexMemorySource = std::make_shared<MemorySource>(std::move(audioIndexData), audioIndexSize);
        auto audioIndexChunk = std::make_shared<RIFFChunk>(avixListMovi.get(), GetFourCC("ix01"), audioIndexMemorySource);
        avixListMovi->AddChild(audioIndexChunk);
        audioStandardIndexChunks.push_back(StandardChunkInfo{
          audioIndexChunk,
          audioDurationCount,
        });
        standardIndices.push_back(std::make_pair(audioIndexMemorySource.get(), &baseRiff));
      }

      if (!isAvix) {
        // idx1
        auto idx1MemorySource = std::make_shared<MemorySource>(sizeof(AVI::AVIINDEXENTRY) * streamCount);
        const auto indexEntries = reinterpret_cast<AVI::AVIINDEXENTRY*>(idx1MemorySource->GetData());
        const auto baseOffset = listMovi->GetOffset() + 8;    // I don't know why +8, but FFmpeg does
        for (std::uint_fast32_t i = 0; i < streamCount; i++) {
          indexEntries[i] = AVI::AVIINDEXENTRY{
            chunks[i].video ? GetFourCC("00dc") : GetFourCC("01wb"),
            AVI::AVIIF_KEYFRAME,                                                      // both video frames and audio blocks are keyframes
            static_cast<std::uint32_t>(chunks[i].chunk->GetOffset() - baseOffset),    // relative to movi (absolute position is permitted also)
            static_cast<std::uint32_t>(chunks[i].chunk->GetSize() - 8),               // I don't know why -8, but FFmpeg does
          };
        }
        idx1->SetContentSource(idx1MemorySource);

        // avih
        reinterpret_cast<AVI::MainAVIHeader*>(avihMemorySource->GetData())->dwTotalFrames = videoFrameCount + 1;    // I don't know why +1, but FFmpeg does
      }

      startNextAvix = true;
    }


    // finish
    if (finished) {
      break;
    }


    if (startNextAvix) {
      // start new RIFF-AVIX list

      riffAvix = std::make_shared<RIFFList>(&mRiffRoot, GetFourCC("RIFF"), GetFourCC("AVIX"));
      mRiffRoot.AddChild(riffAvix);

      avixListMovi = std::make_shared<RIFFList>(riffAvix.get(), GetFourCC("LIST"), GetFourCC("movi"));
      riffAvix->AddChild(avixListMovi);

      videoDataChunks.clear();
      audioDataChunks.clear();
      chunks.clear();

      streamCount = 0;
      sizeCount = static_cast<std::uint_fast32_t>(riffAvix->GetSize());

      videoDurationCount = 0;
      audioDurationCount = 0;
    }


    std::shared_ptr<RIFFChunk> chunk;

    if (isNextChunkVideo) {
      assert(videoFrameCount != videoNumFrames);      // �I�����Ă��Ȃ����Ƃ��m�F

      auto source = std::make_shared<CachedSource>(mCacheStorage, std::make_shared<FrameImageSource>(mMovieFilePlayer, videoFrameCount));
      chunk = std::make_shared<RIFFChunk>(avixListMovi.get(), GetFourCC("00dc"), source);
      videoDataChunks.push_back(chunk);
      chunks.push_back({
        true,
        chunk,
      });
      videoFrameCount++;
      videoDurationCount++;
    } else {
      assert(hasAudio);
      assert(audioSampleCount != audioNumSamples);    // �I�����Ă��Ȃ����Ƃ��m�F

      const std::uint_fast32_t samples = std::min(audioSamplesPerFrame, audioNumSamples - audioSampleCount);
      auto memorySource = std::make_shared<MemorySource>(audioData.data() + audioSampleCount * audioBlockSize, samples * audioBlockSize);
      chunk = std::make_shared<RIFFChunk>(avixListMovi.get(), GetFourCC("01wb"), memorySource);
      audioDataChunks.push_back(chunk);
      chunks.push_back({
        false,
        chunk,
      });
      audioSampleCount += samples;
      audioDurationCount += samples;
    }

    assert(chunk);
    avixListMovi->AddChild(chunk);

    streamCount++;
    sizeCount += static_cast<std::uint_fast32_t>(chunk->GetSize());
  }

  // set indx chunks

  std::vector<std::pair<std::uint64_t*, RIFFChunk*>> superIndexEntries;   // ���qwOffset���C������p

  // video
  {
#if LIKE_FFMPEG
    const std::size_t videoIndexSize = 4120;
#else
    const std::size_t videoIndexSize = sizeof(AVI::AVISUPERINDEX) + sizeof(AVI::AVISUPERINDEXENTRY)* videoStandardIndexChunks.size();
#endif
    auto videoIndexData = std::make_unique<std::uint8_t[]>(videoIndexSize);
    *reinterpret_cast<AVI::AVISUPERINDEX*>(videoIndexData.get()) = AVI::AVISUPERINDEX{
      4u,
      0u,
      0u,   // AVI_INDEX_OF_CHUNKS
      videoStandardIndexChunks.size(),
      GetFourCC("00dc"),
      {},
    };
    const auto videoIndexEntries = reinterpret_cast<AVI::AVISUPERINDEXENTRY*>(videoIndexData.get() + sizeof(AVI::AVISUPERINDEX));
    for (std::uint_fast32_t i = 0; i < videoStandardIndexChunks.size(); i++) {
      auto& videoStandardIndexChunk = videoStandardIndexChunks[i];
      videoIndexEntries[i] = AVI::AVISUPERINDEXENTRY{
        0u,   // filled later
        static_cast<std::uint32_t>(videoStandardIndexChunk.chunk->GetSize()),   // includes the size of a chunk header
        static_cast<std::uint32_t>(videoStandardIndexChunk.duration),
      };

      superIndexEntries.emplace_back(&videoIndexEntries[i].qwOffset, videoStandardIndexChunk.chunk.get());
    }
    videoIndx->SetContentSource(std::make_shared<MemorySource>(std::move(videoIndexData), videoIndexSize));
  }

  // audio
  if (hasAudio) {
#if LIKE_FFMPEG
    const std::size_t audioIndexSize = 4120;
#else
    const std::size_t audioIndexSize = sizeof(AVI::AVISUPERINDEX) + sizeof(AVI::AVISUPERINDEXENTRY) * audioStandardIndexChunks.size();
#endif
    auto audioIndexData = std::make_unique<std::uint8_t[]>(audioIndexSize);
    *reinterpret_cast<AVI::AVISUPERINDEX*>(audioIndexData.get()) = AVI::AVISUPERINDEX{
      4u,
      0u,
      0u,   // AVI_INDEX_OF_CHUNKS
      audioStandardIndexChunks.size(),
      GetFourCC("01wb"),
      {},
    };
    const auto audioIndexEntries = reinterpret_cast<AVI::AVISUPERINDEXENTRY*>(audioIndexData.get() + sizeof(AVI::AVISUPERINDEX));
    for (std::uint_fast32_t i = 0; i < audioStandardIndexChunks.size(); i++) {
      auto& audioStandardIndexChunk = audioStandardIndexChunks[i];
      audioIndexEntries[i] = AVI::AVISUPERINDEXENTRY{
        0u,   // filled later
        static_cast<std::uint32_t>(audioStandardIndexChunk.chunk->GetSize()),   // includes the size of a chunk header
        static_cast<std::uint32_t>(audioStandardIndexChunk.duration),
      };

      superIndexEntries.emplace_back(&audioIndexEntries[i].qwOffset, audioStandardIndexChunk.chunk.get());
    }
    audioIndx->SetContentSource(std::make_shared<MemorySource>(std::move(audioIndexData), audioIndexSize));
  }

  // ���g�݊���

  // fix qwBaseOffset (standard index)
  for (const auto [ptrMemorySource, ptrBaseRiff] : standardIndices) {
    reinterpret_cast<AVI::AVISTDINDEX*>(ptrMemorySource->GetData())->qwBaseOffset = ptrBaseRiff->GetOffset();
  }

  // fix qwOffset (super index)
  for (const auto [ptrQwOffset, ptrStandardIndexChunk] : superIndexEntries) {
    *ptrQwOffset = ptrStandardIndexChunk->GetOffset();
  }

  // ����

#ifdef _DEBUG
  OutputDebugStringW(L"RIFF construction completed\n");
#endif

  mRiffRoot.CreateSource();
}


SourceBase& MEIToAVI::GetSource() {
  return mRiffRoot.GetSource();
}
