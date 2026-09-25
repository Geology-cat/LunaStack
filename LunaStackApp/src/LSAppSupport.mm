#import "LSAppSupport.h"

#include <algorithm>
#include <cmath>

#include "stackcore/fits_writer.hpp"
#include "stackcore/png_writer.hpp"
#include "stackcore/tiff_writer.hpp"

@implementation FlippedView
- (BOOL)isFlipped {
    return YES;
}
@end


void write_output_image(const std::string& path, const stackcore::FrameBuffer& image,
                        OutputFormat format) {
    if (format == OutputFormat::FitsFloat32) {
        stackcore::write_fits_float32(path, image);
        return;
    }
    if (format == OutputFormat::Png16) {
        stackcore::write_png16(path, image);
        return;
    }
    stackcore::write_tiff(path, image, format == OutputFormat::TiffFloat32
                                           ? stackcore::TiffFormat::Float32
                                           : stackcore::TiffFormat::UInt16);
}

NSTextField* MakeLabel(NSString* text) {
    NSTextField* label = [[[NSTextField alloc] init] autorelease];
    [label setStringValue:text];
    [label setBezeled:NO];
    [label setDrawsBackground:NO];
    [label setEditable:NO];
    [label setSelectable:NO];
    [label setFont:[NSFont systemFontOfSize:11.0]];
    [label setTranslatesAutoresizingMaskIntoConstraints:NO];
    return label;
}

JobResult run_job(const JobRequest& req, const stackcore::ProgressFn& progress) {
    JobResult out;
    out.stage = req.stage;
    try {
        std::unique_ptr<stackcore::VideoSource> source =
            stackcore::open_video(req.path, req.options);
        if (req.low_memory) source->set_low_memory(true);

        if (req.stage == JobStage::Quality) {
            out.quality = std::make_shared<stackcore::GlobalStageReport>(
                stackcore::evaluate_frame_quality(*source, req.settings.global,
                                                  req.settings.raw_cfa, progress));
            out.frames = out.quality->frames;
            return out;
        }

        if (req.stage == JobStage::Alignment) {
            if (!req.quality) {
                throw std::runtime_error("アライメント: 先に品質評価を実行してください");
            }
            out.global = std::make_shared<stackcore::GlobalStageReport>(
                stackcore::run_global_alignment(*source, req.settings.global,
                                                req.settings.raw_cfa, *req.quality, progress));
            out.frames = out.global->frames;
            if (!req.global_only) {
                stackcore::MapStackReport report;
                out.analysis = std::make_shared<stackcore::AnalysisData>(
                    stackcore::analyze_map_alignment(*source, req.settings, *out.global,
                                                     progress, report));
                out.frames = out.analysis->frames;
            }
            return out;
        }

        if (req.stage == JobStage::Stack) {
            if (req.global_only) {
                if (!req.global) {
                    throw std::runtime_error("スタック: 先にアライメントを実行してください");
                }
                const std::vector<stackcore::FrameInfo> selected =
                    stackcore::select_top_frames(req.global->frames,
                                                 req.settings.reference_top_percent);
                out.frames = req.global->frames;
                out.image = std::make_shared<stackcore::FrameBuffer>(
                    stackcore::build_global_reference(*source, *req.global, selected,
                                                      req.settings.raw_cfa, progress,
                                                      req.settings.normalize_brightness));
            } else {
                if (!req.analysis) {
                    throw std::runtime_error("スタック: 先にアライメントを実行してください");
                }
                stackcore::MapStackReport report;
                out.analysis = req.analysis;
                out.frames = req.analysis->frames;
                out.image = std::make_shared<stackcore::FrameBuffer>(
                    stackcore::stack_from_analysis(*source, req.settings, *req.analysis,
                                                   progress, report));
            }
            return out;
        }

        // バッチとGUI自己検証用の一括経路。通常のGUIボタンはここを通らない。
        if (req.global_only) {
            out.global = std::make_shared<stackcore::GlobalStageReport>(
                stackcore::run_global_stage(*source, req.settings.global,
                                            req.settings.raw_cfa, progress));
            out.frames = out.global->frames;
            const std::vector<stackcore::FrameInfo> selected = stackcore::select_top_frames(
                out.global->frames, req.settings.reference_top_percent);
            out.image = std::make_shared<stackcore::FrameBuffer>(
                stackcore::build_global_reference(*source, *out.global, selected,
                                                  req.settings.raw_cfa, progress,
                                                  req.settings.normalize_brightness));
        } else {
            stackcore::MapStackReport report;
            out.analysis = std::make_shared<stackcore::AnalysisData>(
                stackcore::analyze_map_stack(*source, req.settings, progress, report));
            out.global =
                std::make_shared<stackcore::GlobalStageReport>(report.global);
            out.frames = out.analysis->frames;
            out.image = std::make_shared<stackcore::FrameBuffer>(stackcore::stack_from_analysis(
                *source, req.settings, *out.analysis, progress, report));
        }
    } catch (const stackcore::Cancelled&) {
        out.cancelled = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

// AP別の平均品質（ヒートマップ用）。行列の並びは points 順 → analyzed_indices 順。
std::vector<double> ap_mean_quality(const stackcore::AnalysisData& analysis) {
    std::vector<double> out;
    const std::size_t ap_count = analysis.points.size();
    const std::size_t frames = analysis.analyzed_indices.size();
    if (ap_count == 0 || frames == 0 || analysis.matrix.size() < ap_count * frames) return out;

    out.resize(ap_count, 0.0);
    for (std::size_t a = 0; a < ap_count; ++a) {
        double sum = 0.0;
        for (std::size_t f = 0; f < frames; ++f) {
            sum += analysis.matrix[a * frames + f].quality;
        }
        out[a] = sum / static_cast<double>(frames);
    }
    return out;
}

