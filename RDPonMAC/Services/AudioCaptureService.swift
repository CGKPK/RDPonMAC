import Foundation
import CoreMedia
import AVFoundation

final class AudioCaptureService {

    var onAudioSamples: ((UnsafePointer<UInt8>, UInt32) -> Void)?

    private var observer: NSObjectProtocol?
    private var converter: AVAudioConverter?
    private var outputFormat: AVAudioFormat?

    func start(sampleRate: Double = 48000, channels: UInt32 = 2) {
        outputFormat = AVAudioFormat(
            commonFormat: .pcmFormatInt16,
            sampleRate: sampleRate,
            channels: AVAudioChannelCount(channels),
            interleaved: true
        )

        observer = NotificationCenter.default.addObserver(
            forName: .screenCaptureAudioFrame,
            object: nil,
            queue: nil
        ) { [weak self] notification in
            guard let userInfo = notification.userInfo,
                  let buffer = userInfo["sampleBuffer"] else { return }
            let sampleBuffer = buffer as! CMSampleBuffer
            self?.processAudioBuffer(sampleBuffer)
        }
    }

    func stop() {
        if let observer = observer {
            NotificationCenter.default.removeObserver(observer)
            self.observer = nil
        }
        converter = nil
    }

    private func processAudioBuffer(_ sampleBuffer: CMSampleBuffer) {
        guard let formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer),
              let outputFormat = outputFormat else { return }

        let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(formatDesc)?.pointee
        guard let sourceASBD = asbd else { return }

        guard let inputFormat = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate: sourceASBD.mSampleRate,
            channels: AVAudioChannelCount(sourceASBD.mChannelsPerFrame),
            interleaved: sourceASBD.mFormatFlags & kAudioFormatFlagIsNonInterleaved == 0
        ) else { return }

        // Create converter if needed or if format changed
        if converter == nil {
            converter = AVAudioConverter(from: inputFormat, to: outputFormat)
        }

        guard let blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) else { return }

        var length: Int = 0
        var dataPointer: UnsafeMutablePointer<Int8>?
        CMBlockBufferGetDataPointer(blockBuffer, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &length, dataPointerOut: &dataPointer)

        guard let data = dataPointer, length > 0 else { return }

        let numFrames = UInt32(CMSampleBufferGetNumSamples(sampleBuffer))
        guard numFrames > 0 else { return }

        // For direct PCM pass-through when formats match, skip conversion
        if inputFormat.sampleRate == outputFormat.sampleRate &&
           inputFormat.channelCount == outputFormat.channelCount {
            // Convert float32 to int16
            let floatCount = length / MemoryLayout<Float>.size
            var int16Data = Data(count: floatCount * MemoryLayout<Int16>.size)

            int16Data.withUnsafeMutableBytes { outPtr in
                let floats = UnsafeBufferPointer(start: UnsafePointer<Float>(OpaquePointer(data)), count: floatCount)
                let int16s = outPtr.bindMemory(to: Int16.self)
                for i in 0..<floatCount {
                    let clamped = max(-1.0, min(1.0, floats[i]))
                    int16s[i] = Int16(clamped * Float(Int16.max))
                }
            }

            int16Data.withUnsafeBytes { rawBuffer in
                guard let ptr = rawBuffer.baseAddress?.assumingMemoryBound(to: UInt8.self) else { return }
                onAudioSamples?(ptr, numFrames)
            }
        }
    }
}
