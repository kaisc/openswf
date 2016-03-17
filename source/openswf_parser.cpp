#include "openswf_debug.hpp"
#include "openswf_parser.hpp"
#include "openswf_charactor.hpp"

namespace openswf
{
    using namespace record;

    Player::Ptr parse(Stream& stream)
    {
        stream.set_position(0);
        auto header = Header::read(stream);
        auto player = Player::Ptr(new Player(header.frame_size, header.frame_rate, header.frame_count));

        auto tag = TagHeader::read(stream);
        while( tag.code != TagCode::END )
        {
            switch(tag.code)
            {
                case TagCode::DEFINE_SHAPE:
                {
                    auto def = DefineShape::read(stream, 1);
                    player->define(def.character_id, new Shape(def));
                    break;
                }

                case TagCode::DEFINE_SHAPE2:
                {
                    auto def = DefineShape::read(stream, 2);
                    player->define(def.character_id, new Shape(def));
                    break;
                }

                case TagCode::DEFINE_SHAPE3:
                {
                    auto def = DefineShape::read(stream, 3);
                    player->define(def.character_id, new Shape(def));
                    break;
                }

                case TagCode::PLACE_OBJECT:
                {
                    auto def = PlaceObject::read(stream, tag.size);
                    player->push_command(new PlaceCommand(def));
                    break;
                }

                case TagCode::PLACE_OBJECT2:
                {
                    auto def = PlaceObject2::read(stream);
                    player->push_command(new PlaceCommand(def));
                    break;
                }

                case TagCode::REMOVE_OBJECT:
                {
                    auto def = RemoveObject::read(stream);
                    player->push_command(new RemoveCommand(def));
                    break;
                }

                case TagCode::SHOW_FRAME:
                {
                    player->record_frame();
                    break;
                }

                default:
                    break;
            }

            stream.set_position(tag.end_pos);
            tag = TagHeader::read(stream);
        }

        return player;
    }

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

        // TAG: 0
        End End::read(Stream& stream)
        {
            return End();
        }

        // TAG: 1
        ShowFrame ShowFrame::read(Stream& stream)
        {
            return ShowFrame();
        }

        // TAG: 2

        void DefineShape::read_line_styles(Stream& stream, LineStyle::Array& array, int type)
        {
            uint8_t count = stream.read_uint8();
            if( count == 0xFF ) count = stream.read_uint16();

            array.reserve(count + array.size());
            for( auto i=0; i<count; i++ )
            {
                LineStyle style;
                style.width = stream.read_uint8();
                if( type >= 3 ) style.rgba = stream.read_rgba();
                else style.rgba = stream.read_rgb();
                array.push_back(style);
            }
        }

        void DefineShape::read_fill_styles(Stream& stream, FillStyle::Array& array, int type)
        {
            uint8_t count = stream.read_uint8();
            if( count == 0xFF ) count = stream.read_uint16();

            array.reserve(count + array.size());
            for( auto i=0; i<count; i++ )
            {
                FillStyle style;
                style.type = (FillStyleCode)stream.read_uint8();

                if( style.type == FillStyleCode::SOLID )
                    if( type >= 3 ) style.rgba = stream.read_rgba();
                    else style.rgba = stream.read_rgb();
                else
                    assert(false); // not supported yet

                array.push_back(style);
            }
        }

        DefineShape DefineShape::read(Stream& stream, int type)
        {
            DefineShape record;

            record.character_id = stream.read_uint16();
            record.bounds       = stream.read_rect();

            read_fill_styles(stream, record.fill_styles, type);
            read_line_styles(stream, record.line_styles, type);

            // parse shape records
            record.fill_index_bits = stream.read_bits_as_uint32(4);
            record.line_index_bits = stream.read_bits_as_uint32(4);

            bool finished = false;
            while( !finished )
            {
                ShapeRecord shape;

                bool is_edge = stream.read_bits_as_uint32(1) > 0;
                if( !is_edge )
                {
                    uint32_t mask = stream.read_bits_as_uint32(5);
                    if( mask == 0x00 )
                    {
                        finished = true;
                        break;
                    }

                    if( mask & 0x01 ) // StateMoveTo
                    {
                        uint8_t bits = stream.read_bits_as_uint32(5);
                        shape.move_delta_x = stream.read_bits_as_int32(bits);
                        shape.move_delta_y = stream.read_bits_as_int32(bits);
                    }

                    if( mask & 0x02 ) // StateFillStyle0
                        shape.fill_style_0 = stream.read_bits_as_uint32(record.fill_index_bits);

                    if( mask & 0x04 ) // StateFillStyle1
                        shape.fill_style_1 = stream.read_bits_as_uint32(record.fill_index_bits);

                    if( mask & 0x08 ) // StateLineStyle
                        shape.line_style = stream.read_bits_as_uint32(record.line_index_bits);

                    if( mask & 0x10 ) // StateNewStyles, used by DefineShape2, DefineShape3 only.
                    {
                        assert( type >= 2 );
                        read_fill_styles(stream, shape.new_fill_styles, type);
                        read_line_styles(stream, shape.new_line_styles, type);
                        shape.fill_index_bits = stream.read_bits_as_uint32(4);
                        shape.line_index_bits = stream.read_bits_as_uint32(4);
                    }
                }
                else
                {
                    bool is_straight = stream.read_bits_as_uint32(1) > 0;
                    if( is_straight )
                    {
                        auto bits = stream.read_bits_as_uint32(4);
                        auto is_general = stream.read_bits_as_uint32(1) > 0;
                        if( is_general )
                        {
                            shape.move_delta_x = stream.read_bits_as_int32(bits+2);
                            shape.move_delta_y = stream.read_bits_as_int32(bits+2);
                            continue;
                        }

                        auto is_vertical = stream.read_bits_as_uint32(1) > 0;
                        if( is_vertical )
                        {
                            shape.move_delta_y = stream.read_bits_as_int32(bits+2);
                            continue;
                        }

                        shape.move_delta_x = stream.read_bits_as_int32(bits+2);
                    }
                    else // curved edges
                        assert(false);
                }

                record.records.push_back(shape);
            }

            return record;
        }

        // TAG: 4
        PlaceObject PlaceObject::read(Stream& stream, int size)
        {
            auto start_pos = stream.get_position();

            PlaceObject record;

            record.character_id  = stream.read_uint16();
            record.depth         = stream.read_uint16();
            record.matrix        = stream.read_matrix();

            if( stream.get_position() < start_pos + size )
                record.cxform    = stream.read_cxform_rgb();

            return record;
        }

        // TAG: 5
        RemoveObject RemoveObject::read(Stream& stream)
        {
            RemoveObject record;
            record.character_id = stream.read_uint16();
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

        // TAG: 26
        PlaceObject2 PlaceObject2::read(Stream& stream)
        {
            PlaceObject2 record;

            auto mask = stream.read_uint8();

            record.depth        = stream.read_uint16();
            record.character_id = mask & PLACE_HAS_CHARACTOR ? stream.read_uint16() : 0;

            if( mask & PLACE_HAS_MATRIX ) record.matrix = stream.read_matrix();
            if( mask & PLACE_HAS_COLOR_TRANSFORM ) record.cxform = stream.read_cxform_rgba();

            record.ratio = mask & PLACE_HAS_RATIO ? stream.read_uint16() : 0;

            if( mask & PLACE_HAS_NAME ) record.name = stream.read_string();
            if( mask & PLACE_HAS_CLIP_DEPTH ) record.clip_depth = stream.read_uint16();
            // if( mask & PLACE_HAS_CLIP_ACTIONS )
                // record.clip_actions = RecordClipActionList::read(stream);

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