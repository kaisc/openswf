#include "openswf_player.hpp"
#include "openswf_parser.hpp"

namespace openswf
{
    PlaceCommand::PlaceCommand(const record::PlaceObject& def)
        : character_id(def.character_id), depth(def.depth), transform(def.matrix), cxform(def.cxform) {}

    PlaceCommand::PlaceCommand(const record::PlaceObject2& def)
        : character_id(def.character_id), depth(def.depth), transform(def.matrix), cxform(def.cxform) {}

    void PlaceCommand::execute(Player* player)
    {
        player->place(character_id, depth, transform, cxform);
    }

    RemoveCommand::RemoveCommand(const record::RemoveObject& def)
        : character_id(def.character_id), depth(def.depth) {}

    void RemoveCommand::execute(Player* player)
    {
        player->remove(character_id, depth);
    }

    Player::~Player()
    {
        for( auto command : m_records )
            delete command;

        for( auto& frame : m_frames )
            for( auto command : frame )
                delete command;

        for( auto& pair : m_dictionary )
            delete pair.second;
    }

    void Player::update(float dt)
    {
        if( m_current_frame >= m_frames.size() )
            return;

        auto frame_delta = 1 / m_frame_rate;
        m_timer += dt;

        while( m_timer > frame_delta )
        {
            m_timer -= frame_delta;
            m_current_frame ++;

            if( m_current_frame >= m_frames.size() )
                break;

            for( auto command : m_frames[m_current_frame] )
                command->execute(this);
        }
    }

    void Player::render()
    {
        for( auto& pair : m_displays )
            pair.second->render();
    }
}