#include "debug.hpp"
#include "record.hpp"
#include "charactor.hpp"

#include <unordered_map>

namespace openswf
{
    namespace record
    {
        Header Header::read(Stream& stream)
        {
            Header record;
            record.compressed   = (char)stream.read_uint8() != 'F';
            auto const_w        = stream.read_uint8();
            auto const_s        = stream.read_uint8();
            record.version      = stream.read_uint8();
            record.size         = stream.read_uint32();

            assert( !record.compressed ); // compressed mode not supports
            assert( (char)const_w == 'W' && (char)const_s == 'S' );

            record.frame_size    = stream.read_rect();
            record.frame_rate    = stream.read_fixed16();
            record.frame_count   = stream.read_uint16();

            // some SWF files have been seen that have 0-frame sprites.
            // but the macromedia player behaves as if they have 1 frame.
            record.frame_count   = std::max(record.frame_count, (uint16_t)1);
            return record;
        }

        TagHeader TagHeader::read(Stream& stream)
        {
            TagHeader record;

            uint32_t header = stream.read_uint16();
            record.code     = (TagCode)(header >> 6);
            record.size     = header & 0x3f;

            // if the tag is 63 bytes or longer, it is stored in a long tag header.
            if( record.size == 0x3f )
                record.size = (uint32_t)stream.read_int32();

            record.end_pos = stream.get_position() + record.size;
            return record;
        }

        // TAG: 2, 22, 32, 83
        enum class FillStyleCode : uint8_t
        {
            SOLID                           = 0x00,
            LINEAR_GRADIENT                 = 0x10,
            RADIAL_GRADIENT                 = 0x12,
            FOCAL_RADIAL_GRADIENT           = 0x13,

            REPEATING_BITMAP                = 0x40,
            CLIPPED_BITMAP                  = 0x41,
            NON_SMOOTHED_REPEATING_BITMAP   = 0x42,
            NON_SMOOTHED_CLIPPED_BITMAP     = 0x43
        };

        static void read_gradient(Stream& stream, GradientFill* gradient, TagCode tag)
        {
            gradient->transform = stream.read_matrix();
            gradient->spread = (GradientFill::SpreadMode)stream.read_bits_as_uint32(2);
            gradient->interp = (GradientFill::InterpolationMode)stream.read_bits_as_uint32(2);

            auto count = stream.read_bits_as_uint32(4);
            assert( count > 0 );

            gradient->controls.reserve(count);
            for( auto i=0; i<count; i++ )
            {
                GradientFill::ControlPoint ctrl;
                ctrl.ratio = stream.read_uint8();
                if( tag == TagCode::DEFINE_SHAPE3 || tag == TagCode::DEFINE_SHAPE4 )
                    ctrl.color = stream.read_rgba();
                else 
                    ctrl.color = stream.read_rgb();
                gradient->controls.push_back(ctrl);
            }

            // WARNING: do we need to sort controls by ratio ?
        }

        static FillStyle* read_fill_style(Stream& stream, TagCode tag)
        {
            auto type = (FillStyleCode)stream.read_uint8();
            if( type == FillStyleCode::SOLID )
            {
                auto solid = new SolidFill();
                if( tag == TagCode::DEFINE_SHAPE3 || tag == TagCode::DEFINE_SHAPE4 )
                    solid->color = stream.read_rgba();
                else 
                    solid->color = stream.read_rgb();
                return solid;
            }
            else if( type == FillStyleCode::LINEAR_GRADIENT )
            {
                auto linear = new LinearGradientFill();
                read_gradient(stream, linear, tag);
                return linear;
            }
            else if( type == FillStyleCode::RADIAL_GRADIENT )
            {
                auto radial = new RadialGradientFill();
                read_gradient(stream, radial, tag);
                return radial;
            }
            else if( type == FillStyleCode::FOCAL_RADIAL_GRADIENT )
            {
                auto focal = new FocalRadialGradientFill();
                read_gradient(stream, focal, tag);
                focal->focal = stream.read_fixed16();
                return focal;
            }
                assert(false); // not supported yet
        }

        static void read_line_styles(Stream& stream, LineStyle::Array& array, TagCode type)
        {
            uint8_t count = stream.read_uint8();
            if( count == 0xFF ) count = stream.read_uint16();

            array.reserve(count + array.size());
            for( auto i=0; i<count; i++ )
            {
                LineStyle line;
                line.width = stream.read_uint16();

                if( type == TagCode::DEFINE_SHAPE4 )
                {   // line style 2
                    line.start_cap  = (Capcode)stream.read_bits_as_uint32(2);
                    line.join       = (Joincode)stream.read_bits_as_uint32(2);
                    line.has_fill   = stream.read_bits_as_uint32(1) > 0;
                    line.no_hscale  = stream.read_bits_as_uint32(1) > 0;
                    line.no_vscale  = stream.read_bits_as_uint32(1) > 0;
                    line.pixel_hinting = stream.read_bits_as_uint32(1) > 0;

                    assert( stream.read_bits_as_uint32(5) == 0 ); //reserved bits
                    
                    line.no_close    = stream.read_bits_as_uint32(1) > 0;
                    line.end_cap    = (Capcode)stream.read_bits_as_uint32(2);
                    line.miter_limit_factor = line.join == Joincode::MITER ? stream.read_uint16() : 0;
                    
                    line.fill = nullptr;
                    if( line.has_fill )
                        read_fill_style(stream, type);
                    else
                        line.color = stream.read_rgba();
                }
                else
                {   // line style
                    if( type == TagCode::DEFINE_SHAPE3 )
                        line.color = stream.read_rgba();
                    else
                        line.color = stream.read_rgb();
                }
                array.push_back(line);
            }
        }

        static void read_fill_styles(Stream& stream, std::vector<FillPtr>& array, TagCode tag)
        {
            uint8_t count = stream.read_uint8();
            if( count == 0xFF ) count = stream.read_uint16();

            array.reserve(count + array.size());
            for( auto i=0; i<count; i++ )
                array.push_back(std::unique_ptr<FillStyle>(read_fill_style(stream, tag)));
        }

        enum DefineShapeMask
        {
            SHAPE_END           = 0x00,
            SHAPE_MOVE_TO       = 0x01,
            SHAPE_FILL_STYLE_0  = 0x02,
            SHAPE_FILL_STYLE_1  = 0x04,
            SHAPE_LINE_STYLE    = 0x08,
            SHAPE_NEW_STYLE     = 0x10
        };

        DefineShape DefineShape::read(Stream& stream, TagCode type)
        {
            assert( 
                type == TagCode::DEFINE_SHAPE ||
                type == TagCode::DEFINE_SHAPE2 ||
                type == TagCode::DEFINE_SHAPE3 ||
                type == TagCode::DEFINE_SHAPE4 );

            DefineShape record;

            record.character_id = stream.read_uint16();
            record.bounds       = stream.read_rect();

            if( type == TagCode::DEFINE_SHAPE4 )
            {
                record.edge_bounds = stream.read_rect();
                stream.read_bits_as_uint32(5);
                stream.read_bits_as_uint32(1);
                stream.read_bits_as_uint32(1);
                stream.read_bits_as_uint32(1);
            }

            read_fill_styles(stream, record.fill_styles, type);

            std::vector<LineStyle> line_styles;
            read_line_styles(stream, line_styles, type);

            // parse shape records
            uint32_t fill_index_bits = stream.read_bits_as_uint32(4);
            uint32_t line_index_bits = stream.read_bits_as_uint32(4);
            uint32_t fill_index_base = 0, line_index_base = 0;
            Point2f cursor;

            ShapePath current_path;
            auto push_path = [&](bool reset = false)
            {
                if( !current_path.edges.empty() )
                {
                    record.paths.push_back(current_path);
                }

                current_path.restart(cursor);
                if( reset ) current_path.reset();
            };

            while( true )
            {
                bool is_edge = stream.read_bits_as_uint32(1) > 0;
                if( !is_edge )
                {
                    uint32_t mask = stream.read_bits_as_uint32(5);
                    if( mask == SHAPE_END ) // EndShapeRecord
                    {
                        push_path();
                        break;
                    }

                    // StyleChangeRecord
                    if( mask & SHAPE_MOVE_TO ) // StateMoveTo
                    {
                        uint8_t bits = stream.read_bits_as_uint32(5);
                        cursor.x = (float)stream.read_bits_as_int32(bits);
                        cursor.y = (float)stream.read_bits_as_int32(bits);

                        push_path(true);
                    }

                    if( (mask & SHAPE_FILL_STYLE_0) && fill_index_bits > 0 ) // StateFillStyle0
                    {
                        push_path();

                        current_path.left_fill = stream.read_bits_as_uint32(fill_index_bits);
                        if( current_path.left_fill > 0 )
                            current_path.left_fill += fill_index_base;
                    }

                    if( (mask & SHAPE_FILL_STYLE_1) && fill_index_bits > 0 ) // StateFillStyle1
                    {
                        push_path();
                        current_path.right_fill = stream.read_bits_as_uint32(fill_index_bits);
                        if( current_path.right_fill > 0 )
                            current_path.right_fill += fill_index_base;
                    }

                    if( (mask & SHAPE_LINE_STYLE) && line_index_bits > 0 ) // StateLineStyle
                    {
                        push_path();
                        current_path.line = stream.read_bits_as_uint32(line_index_bits);
                    }

                    if( mask & SHAPE_NEW_STYLE ) // StateNewStyles, used by DefineShape2, DefineShape3 only.
                    {
                        assert( type == TagCode::DEFINE_SHAPE3 || type == TagCode::DEFINE_SHAPE4 );
                        push_path();

                        fill_index_base = record.fill_styles.size();
                        line_index_base = line_styles.size();
                        read_fill_styles(stream, record.fill_styles, type);
                        read_line_styles(stream, line_styles, type);
                        fill_index_bits = stream.read_bits_as_uint32(4);
                        line_index_bits = stream.read_bits_as_uint32(4);
                    }
                }
                else
                {
                    bool is_straight = stream.read_bits_as_uint32(1) > 0;
                    if( is_straight ) // StraightEdgeRecrod
                    {
                        float dx = 0, dy = 0;
                        auto bits = stream.read_bits_as_uint32(4) + 2;
                        auto is_general = stream.read_bits_as_uint32(1) > 0;
                        if( is_general )
                        {
                            dx = (float)stream.read_bits_as_int32(bits);
                            dy = (float)stream.read_bits_as_int32(bits);
                        }
                        else
                        {
                            auto is_vertical = stream.read_bits_as_uint32(1) > 0;
                            if( is_vertical )
                                dy = (float)stream.read_bits_as_int32(bits);
                            else
                                dx = (float)stream.read_bits_as_int32(bits);
                        }

                        cursor.x += dx;
                        cursor.y += dy;

                        current_path.edges.push_back(ShapeEdge(cursor));
                    }
                    else // CurvedEdgeRecord
                    {
                        auto bits   = stream.read_bits_as_uint32(4) + 2;
                        auto cx     = (float)stream.read_bits_as_int32(bits);
                        auto cy     = (float)stream.read_bits_as_int32(bits);
                        auto ax     = (float)stream.read_bits_as_int32(bits);
                        auto ay     = (float)stream.read_bits_as_int32(bits);

                        current_path.edges.push_back(ShapeEdge(cx, cy, ax, ay));
                        cursor.x = ax;
                        cursor.y = ay;
                    }
                }
            }

            return record;
        }

        // TAG: 4, 26
        PlaceObject PlaceObject::read(Stream& stream, const TagHeader& header)
        {
            assert(header.code == TagCode::PLACE_OBJECT || header.code == TagCode::PLACE_OBJECT2);

            PlaceObject record;
            if( header.code == TagCode::PLACE_OBJECT )
                record.parse_tag_4(stream, header);
            else
                record.parse_tag_26(stream);

            return record;
        }

        void PlaceObject::parse_tag_4(Stream& stream, const TagHeader& header)
        {
            this->character_id  = stream.read_uint16();
            this->depth         = stream.read_uint16();
            this->matrix        = stream.read_matrix();

            if( stream.get_position() < header.end_pos )
                this->cxform    = stream.read_cxform_rgb();
        }

        enum Place2Mask
        {
            PLACE_2_HAS_MOVE        = 0x01,
            PLACE_2_HAS_CHARACTOR   = 0x02,
            PLACE_2_HAS_MATRIX      = 0x04,
            PLACE_2_HAS_CXFORM      = 0x08,
            PLACE_2_HAS_RATIO       = 0x10,
            PLACE_2_HAS_NAME        = 0x20,
            PLACE_2_HAS_CLIP_DEPTH  = 0x40,
            PLACE_2_HAS_CLIP_ACTIONS= 0x80
        };

        void PlaceObject::parse_tag_26(Stream& stream)
        {
            auto mask = stream.read_uint8();

            this->depth        = stream.read_uint16();
            this->character_id = mask & PLACE_2_HAS_CHARACTOR ? stream.read_uint16() : 0;

            if( mask & PLACE_2_HAS_MATRIX ) this->matrix = stream.read_matrix();
            if( mask & PLACE_2_HAS_CXFORM ) this->cxform = stream.read_cxform_rgba();

            this->ratio = mask & PLACE_2_HAS_RATIO ? stream.read_uint16() : 0;

            if( mask & PLACE_2_HAS_NAME ) this->name = stream.read_string();
            if( mask & PLACE_2_HAS_CLIP_DEPTH ) this->clip_depth = stream.read_uint16();
            
            // if( mask & PLACE_HAS_CLIP_ACTIONS )
                // record.clip_actions = RecordClipActionList::read(stream);
        }

        enum Place3Mask
        {
            PLACE_3_HAS_FILTERS         = 0x0001,
            PLACE_3_HAS_BLEND_MODE      = 0x0002,
            PLACE_3_HAS_CACHE_AS_BITMAP = 0x0004,
            PLACE_3_HAS_CLASS_NAME      = 0x0008,
            PLACE_3_HAS_IMAGE           = 0x0010,

            PLACE_3_RESERVED_1          = 0x0020,
            PLACE_3_RESERVED_2          = 0x0040,
            PLACE_3_RESERVED_3          = 0x0080,
            PLACE_3_MOVE                = 0x0100,
            PLACE_3_HAS_CHARACTOR       = 0x0200,
            PLACE_3_HAS_MATRIX          = 0x0400,
            PLACE_3_HAS_CXFORM          = 0x0800,
            PLACE_3_HAS_RATIO           = 0x1000,
            PLACE_3_HAS_NAME            = 0x2000,
            PLACE_3_HAS_CLIP_DEPTH      = 0x4000,
            PLACE_3_HAS_CLIPS           = 0x8000
        };

        // TAG: 5
        RemoveObject RemoveObject::read(Stream& stream, TagCode type)
        {
            assert( type == TagCode::REMOVE_OBJECT || type == TagCode::REMOVE_OBJECT2 );

            RemoveObject record;
            record.character_id = type == TagCode::REMOVE_OBJECT ? stream.read_uint16() : 0;
            record.depth        = stream.read_uint16();
            return record;
        }

        // TAG: 9
        SetBackgroundColor SetBackgroundColor::read(Stream& stream)
        {
            SetBackgroundColor record;
            record.color = stream.read_rgb();
            return record;
        }

        // TAG: 39
        DefineSpriteHeader DefineSpriteHeader::read(Stream& stream)
        {
            DefineSpriteHeader record;
            record.character_id = stream.read_uint16();
            record.frame_count = stream.read_uint16();

            return record;
        }

        // TAG: 43
        FrameLabel FrameLabel::read(Stream& stream)
        {
            FrameLabel record;
            record.name = stream.read_string();
            record.named_anchor = stream.read_uint8();
            return record;
        }

        // TAG: 69
        FileAttributes FileAttributes::read(Stream& stream)
        {
            FileAttributes record;
            record.attributes = stream.read_uint32();
            return record;
        }

        // TAG: 86
        DefineSceneAndFrameLabelData DefineSceneAndFrameLabelData::read(Stream& stream)
        {
            DefineSceneAndFrameLabelData record;
            record.scene_count = stream.read_encoded_uint32();
            for( auto i=0; i<record.scene_count; i++ )
            {
                record.scene_offsets.push_back(stream.read_encoded_uint32());
                record.scene_names.push_back(stream.read_string());
            }

            record.frame_label_count = stream.read_encoded_uint32();
            for( auto i=0; i<record.frame_label_count; i++ )
            {
                record.frame_numbers.push_back(stream.read_encoded_uint32());
                record.frame_labels.push_back(stream.read_string());
            }

            return record;
        }
    }
}