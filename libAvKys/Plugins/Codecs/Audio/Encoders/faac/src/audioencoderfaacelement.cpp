/* Webcamoid, camera capture application.
 * Copyright (C) 2024  Gonzalo Exequiel Pedone
 *
 * Webcamoid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Webcamoid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Webcamoid. If not, see <http://www.gnu.org/licenses/>.
 *
 * Web-Site: http://webcamoid.github.io/
 */

#include <QBitArray>
#include <QCoreApplication>
#include <QMutex>
#include <QVariant>
#include <akfrac.h>
#include <akpacket.h>
#include <akaudiocaps.h>
#include <akcompressedaudiocaps.h>
#include <akaudiopacket.h>
#include <akcompressedaudiopacket.h>
#include <akpluginmanager.h>
#include <iak/akelement.h>
#include <faac.h>

#include "audioencoderfaacelement.h"

struct FaacSampleFormatTable
{
    AkAudioCaps::SampleFormat format;
    int faacFormat;

    static inline const FaacSampleFormatTable *table()
    {
        static const FaacSampleFormatTable faacSampleFormatTable[] = {
            {AkAudioCaps::SampleFormat_s16 , FAAC_INPUT_16BIT},
            {AkAudioCaps::SampleFormat_s32 , FAAC_INPUT_32BIT},
            //{AkAudioCaps::SampleFormat_flt , FAAC_INPUT_FLOAT}, // Not in the range [-1.0, 1.0]
            {AkAudioCaps::SampleFormat_none, FAAC_INPUT_NULL },
        };

        return faacSampleFormatTable;
    }

    static inline const FaacSampleFormatTable *byFormat(AkAudioCaps::SampleFormat format)
    {
        auto fmt = table();

        for (; fmt->format != AkAudioCaps::SampleFormat_none; fmt++)
            if (fmt->format == format)
                return fmt;

        return fmt;
    }

    static inline const FaacSampleFormatTable *byFaacFormat(int faacFormat)
    {
        auto fmt = table();

        for (; fmt->format != AkAudioCaps::SampleFormat_none; fmt++)
            if (fmt->faacFormat == faacFormat)
                return fmt;

        return fmt;
    }
};

class AudioEncoderFaacElementPrivate
{
    public:
        AudioEncoderFaacElement *self;
        AkCompressedAudioCaps m_outputCaps;
        AkPropertyOptions m_options;
        QByteArray m_headers;
        faacEncHandle m_encoder {nullptr};
        faacEncConfigurationPtr m_config {nullptr};
        unsigned long m_maxOutputBytes {0};
        QMutex m_mutex;
        bool m_initialized {false};
        bool m_paused {false};
        qint64 m_id {0};
        int m_index {0};
        qint64 m_pts {0};
        qint64 m_encodedTimePts {0};
        AkElementPtr m_fillAudioGaps {akPluginManager->create<AkElement>("AudioFilter/FillAudioGaps")};

        explicit AudioEncoderFaacElementPrivate(AudioEncoderFaacElement *self);
        ~AudioEncoderFaacElementPrivate();
        static int nearestSampleRate(int rate);
        static int sampleRateIndex(int rate);
        static void putBits(QBitArray &ba, qsizetype bits, quint32 value);
        static QByteArray bitsToByteArray(const QBitArray &bits);
        bool init();
        void uninit();
        void updateHeaders();
        void updateOutputCaps(const AkAudioCaps &inputCaps);
        void encodeFrame(const AkAudioPacket &src);
        void sendFrame(const QByteArray &packetData,
                       qsizetype samples,
                       qsizetype writtenBytes);
};

AudioEncoderFaacElement::AudioEncoderFaacElement():
    AkAudioEncoder()
{
    this->d = new AudioEncoderFaacElementPrivate(this);
    this->setCodec(this->codecs().value(0));
}

AudioEncoderFaacElement::~AudioEncoderFaacElement()
{
    this->d->uninit();
    delete this->d;
}

QStringList AudioEncoderFaacElement::codecs() const
{
    return {"faac"};
}

AkAudioEncoderCodecID AudioEncoderFaacElement::codecID(const QString &codec) const
{
    return codec == this->codecs().first()?
                AkCompressedAudioCaps::AudioCodecID_aac:
                AkCompressedAudioCaps::AudioCodecID_unknown;
}

QString AudioEncoderFaacElement::codecDescription(const QString &codec) const
{
    return codec == this->codecs().first()?
                QStringLiteral("AAC (faac)"):
                QString();
}

AkCompressedAudioCaps AudioEncoderFaacElement::outputCaps() const
{
    return this->d->m_outputCaps;
}

QByteArray AudioEncoderFaacElement::headers() const
{
    return this->d->m_headers;
}

qint64 AudioEncoderFaacElement::encodedTimePts() const
{
    return this->d->m_encodedTimePts;
}

AkPropertyOptions AudioEncoderFaacElement::options() const
{
    return this->d->m_options;
}

AkPacket AudioEncoderFaacElement::iAudioStream(const AkAudioPacket &packet)
{
    QMutexLocker mutexLocker(&this->d->m_mutex);

    if (this->d->m_paused
        || !this->d->m_initialized
        || !this->d->m_fillAudioGaps)
        return {};

    this->d->m_fillAudioGaps->iStream(packet);

    return {};
}

bool AudioEncoderFaacElement::setState(ElementState state)
{
    auto curState = this->state();

    switch (curState) {
    case AkElement::ElementStateNull: {
        switch (state) {
        case AkElement::ElementStatePaused:
            this->d->m_paused = state == AkElement::ElementStatePaused;
        case AkElement::ElementStatePlaying:
            if (!this->d->init()) {
                this->d->m_paused = false;

                return false;
            }

            return AkElement::setState(state);
        default:
            break;
        }

        break;
    }
    case AkElement::ElementStatePaused: {
        switch (state) {
        case AkElement::ElementStateNull:
            this->d->uninit();

            return AkElement::setState(state);
        case AkElement::ElementStatePlaying:
            this->d->m_paused = false;

            return AkElement::setState(state);
        default:
            break;
        }

        break;
    }
    case AkElement::ElementStatePlaying: {
        switch (state) {
        case AkElement::ElementStateNull:
            this->d->uninit();

            return AkElement::setState(state);
        case AkElement::ElementStatePaused:
            this->d->m_paused = true;

            return AkElement::setState(state);
        default:
            break;
        }

        break;
    }
    }

    return false;
}

AudioEncoderFaacElementPrivate::AudioEncoderFaacElementPrivate(AudioEncoderFaacElement *self):
    self(self)
{
    this->m_options = {
        {"mpegVersion" ,
         QObject::tr("MPEG version"),
         "",
         AkPropertyOption::OptionType_Number,
         0.0,
         1.0,
         1.0,
         0.0,
         {{"mpeg4", QObject::tr("MPEG version 4"), "", 0.0},
          {"mpeg2", QObject::tr("MPEG version 2"), "", 1.0}}},
        {"outputFormat",
         QObject::tr("Output format"),
         "",
         AkPropertyOption::OptionType_Number,
         0.0,
         1.0,
         1.0,
         0.0,
         {{"raw" , QObject::tr("Raw") , "", 0.0},
          {"adts", QObject::tr("ADTS"), "", 1.0}}},
    };

    if (this->m_fillAudioGaps)
        QObject::connect(this->m_fillAudioGaps.data(),
                         &AkElement::oStream,
                         [this] (const AkPacket &packet) {
                             this->encodeFrame(packet);
                         });

    QObject::connect(self,
                     &AkAudioEncoder::inputCapsChanged,
                     [this] (const AkAudioCaps &inputCaps) {
                         this->updateOutputCaps(inputCaps);
                     });
}

AudioEncoderFaacElementPrivate::~AudioEncoderFaacElementPrivate()
{

}

int AudioEncoderFaacElementPrivate::nearestSampleRate(int rate)
{
    static const int faacEncSupportedSampleRates[] = {
        8000,
        11025,
        12000,
        16000,
        22050,
        24000,
        32000,
        44100,
        48000,
        64000,
        88200,
        96000,
        0
    };

    int nearest = rate;
    quint64 q = std::numeric_limits<quint64>::max();

    for (auto srate = faacEncSupportedSampleRates; *srate; ++srate) {
        quint64 k = qAbs(*srate - rate);

        if (k < q) {
            nearest = *srate;
            q = k;
        }
    }

    return nearest;
}

int AudioEncoderFaacElementPrivate::sampleRateIndex(int rate)
{
    static const int faacEncSampleRateIndex[] = {
        96000,
        88200,
        64000,
        48000,
        44100,
        32000,
        24000,
        22050,
        16000,
        12000,
        11025,
        8000,
        7350,
        0
    };

    for (int i = 0; faacEncSampleRateIndex[i]; ++i)
        if (faacEncSampleRateIndex[i] == rate)
            return i;

    return 15;
}

void AudioEncoderFaacElementPrivate::putBits(QBitArray &ba,
                                             qsizetype bits,
                                             quint32 value)
{
    ba.resize(ba.size() + bits);

    for (qsizetype i = 0; i < bits; ++i)
        ba[ba.size() - i - 1] = (value >> i) & 0x1;
}

QByteArray AudioEncoderFaacElementPrivate::bitsToByteArray(const QBitArray &bits)
{
    QByteArray bytes((bits.size() + 7) / 8, 0);

    for (int i = 0; i < bits.size(); ++i)
        bytes[i / 8] |= (bits[i]? 1: 0) << (7 - (i % 8));

    return bytes;
}

bool AudioEncoderFaacElementPrivate::init()
{
    this->uninit();

    auto inputCaps = self->inputCaps();

    if (!inputCaps) {
        qCritical() << "Invalid input format.";

        return false;
    }

    unsigned long inputSamples = 0;
    this->m_encoder = faacEncOpen(this->m_outputCaps.rawCaps().rate(),
                                  this->m_outputCaps.rawCaps().channels(),
                                  &inputSamples,
                                  &this->m_maxOutputBytes);

    if (!this->m_encoder) {
        qCritical() << "Failed opening the encoder";

        return false;
    }

    this->m_config = faacEncGetCurrentConfiguration(this->m_encoder);

    if (this->m_config->version != FAAC_CFG_VERSION) {
        qCritical() << "Wrong libfaac version";
        faacEncClose(this->m_encoder);

        return false;
    }

    this->m_config->aacObjectType = LOW; // This is the only supported type in the library
    this->m_config->mpegVersion = self->optionValue("mpegVersion").toUInt();
    this->m_config->useTns = 0;
    this->m_config->allowMidside = 1;
    this->m_config->bitRate = self->bitrate() / this->m_outputCaps.rawCaps().channels();
    this->m_config->bandWidth = 0;
    this->m_config->outputFormat = self->optionValue("outputFormat").toUInt();
    this->m_config->inputFormat =
            FaacSampleFormatTable::byFormat(this->m_outputCaps
                                            .rawCaps()
                                            .format())->faacFormat;

    if (!faacEncSetConfiguration(this->m_encoder, this->m_config)) {
        qCritical() << "Error setting configs";
        faacEncClose(this->m_encoder);

        return false;
    }

    this->updateHeaders();

    if (this->m_fillAudioGaps) {
        this->m_fillAudioGaps->setProperty("fillGaps", self->fillGaps());
        this->m_fillAudioGaps->setProperty("outputSamples",
                                           int(inputSamples
                                               / this->m_outputCaps.rawCaps().channels()));
        this->m_fillAudioGaps->setState(AkElement::ElementStatePlaying);
    }

    this->m_pts = 0;
    this->m_encodedTimePts = 0;
    this->m_initialized = true;

    return true;
}

void AudioEncoderFaacElementPrivate::uninit()
{
    QMutexLocker mutexLocker(&this->m_mutex);

    if (!this->m_initialized)
        return;

    this->m_initialized = false;

    if (this->m_fillAudioGaps)
        this->m_fillAudioGaps->setState(AkElement::ElementStateNull);

    if (this->m_encoder) {
        faacEncClose(this->m_encoder);
        this->m_encoder = nullptr;
    }

    this->m_paused = false;
}

void AudioEncoderFaacElementPrivate::updateHeaders()
{
    // https://wiki.multimedia.cx/index.php/MPEG-4_Audio
    // https://csclub.uwaterloo.ca/~pbarfuss/ISO14496-3-2009.pdf
    // https://learn.microsoft.com/es-es/windows/win32/medfound/aac-decoder

    QBitArray audioSpecificConfig;

    // Set audio specific config
    putBits(audioSpecificConfig, 5, this->m_config->aacObjectType);
    auto sri = sampleRateIndex(this->m_outputCaps.rawCaps().rate());
    putBits(audioSpecificConfig, 4, sri);

    if (sri >= 15)
        putBits(audioSpecificConfig, 24, this->m_outputCaps.rawCaps().rate());

    putBits(audioSpecificConfig, 4, this->m_outputCaps.rawCaps().channels());

    // Set GASpecificConfig
    putBits(audioSpecificConfig, 1, 0); //frame length - 1024 samples
    putBits(audioSpecificConfig, 1, 0); //does not depend on core coder
    putBits(audioSpecificConfig, 1, 0); //is not extension

    // Disable SBR
    putBits(audioSpecificConfig, 11, 0x2b7);
    putBits(audioSpecificConfig, 5, 5);
    putBits(audioSpecificConfig, 1, 0);

    audioSpecificConfig.resize(32 * 8);
    auto headers = bitsToByteArray(audioSpecificConfig);

    if (this->m_headers == headers)
        return;

    this->m_headers = headers;
    emit self->headersChanged(headers);
}

void AudioEncoderFaacElementPrivate::updateOutputCaps(const AkAudioCaps &inputCaps)
{
    if (!inputCaps) {
        if (!this->m_outputCaps)
            return;

        this->m_outputCaps = AkCompressedAudioCaps();
        emit self->outputCapsChanged(this->m_outputCaps);

        return;
    }

    auto codecID = self->codecID(self->codec());

    if (codecID == AkCompressedAudioCaps::AudioCodecID_unknown) {
        if (!this->m_outputCaps)
            return;

        this->m_outputCaps = AkCompressedAudioCaps();
        emit self->outputCapsChanged(this->m_outputCaps);

        return;
    }

    auto eqFormat = FaacSampleFormatTable::byFormat(inputCaps.format());

    if (eqFormat->format == AkAudioCaps::SampleFormat_none)
        eqFormat = FaacSampleFormatTable::byFormat(AkAudioCaps::SampleFormat_s16);

    int channels = qBound(1, inputCaps.channels(), 2);
    int rate = nearestSampleRate(inputCaps.rate());
    AkAudioCaps rawCaps(eqFormat->format,
                        AkAudioCaps::defaultChannelLayout(channels),
                        false,
                        rate);
    AkCompressedAudioCaps outputCaps(codecID, rawCaps);

    if (this->m_fillAudioGaps)
        this->m_fillAudioGaps->setProperty("outputCaps",
                                           QVariant::fromValue(rawCaps));

    if (this->m_outputCaps == outputCaps)
        return;

    this->m_outputCaps = outputCaps;
    emit self->outputCapsChanged(outputCaps);
}

void AudioEncoderFaacElementPrivate::encodeFrame(const AkAudioPacket &src)
{
    if (!src)
        return;

    this->m_id = src.id();
    this->m_index = src.index();

    QByteArray packetData(this->m_maxOutputBytes, Qt::Uninitialized);
    auto writtenBytes =
            faacEncEncode(this->m_encoder,
                          const_cast<int32_t *>(reinterpret_cast<const int32_t *>(src.constData())),
                          src.samples() * src.caps().channels(),
                          reinterpret_cast<unsigned char *>(packetData.data()),
                          packetData.size());

    if (writtenBytes < 0) {
        qCritical() << "Failed encoding the samples:" << writtenBytes;

        return;
    } else if (writtenBytes > 0) {
        this->sendFrame(packetData, src.samples(), writtenBytes);
    }

    this->m_encodedTimePts += src.samples();
    emit self->encodedTimePtsChanged(this->m_encodedTimePts);
}

void AudioEncoderFaacElementPrivate::sendFrame(const QByteArray &packetData,
                                               qsizetype samples,
                                               qsizetype writtenBytes)
{
    AkCompressedAudioPacket packet(this->m_outputCaps, writtenBytes);
    memcpy(packet.data(), packetData.constData(), packet.size());
    packet.setPts(this->m_pts);
    packet.setDts(this->m_pts);
    packet.setDuration(samples);
    packet.setTimeBase({1, this->m_outputCaps.rawCaps().rate()});
    packet.setId(this->m_id);
    packet.setIndex(this->m_index);

    emit self->oStream(packet);

    this->m_pts += samples;
}

#include "moc_audioencoderfaacelement.cpp"
