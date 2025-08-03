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

#ifndef  VIDEOENCODERX264ELEMENT_H
#define  VIDEOENCODERX264ELEMENT_H

#include <iak/akvideoencoder.h>

class VideoEncoderX264ElementPrivate;

class VideoEncoderX264Element: public AkVideoEncoder
{
    Q_OBJECT

    public:
        VideoEncoderX264Element();
        ~VideoEncoderX264Element();

        Q_INVOKABLE QStringList codecs() const override;
        Q_INVOKABLE AkVideoEncoderCodecID codecID(const QString &codec) const override;
        Q_INVOKABLE QString codecDescription(const QString &codec) const override;
        Q_INVOKABLE AkCompressedVideoCaps outputCaps() const override;
        Q_INVOKABLE QByteArray headers() const override;
        Q_INVOKABLE qint64 encodedTimePts() const override;
        Q_INVOKABLE AkPropertyOptions options() const override;

    private:
        VideoEncoderX264ElementPrivate *d;

    protected:
        AkPacket iVideoStream(const AkVideoPacket &packet) override;

    public slots:
        bool setState(AkElement::ElementState state) override;
};

#endif // VIDEOENCODERX264ELEMENT_H
