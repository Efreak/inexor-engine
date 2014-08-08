#include "engine.h"

VAR(importcuberemip, 0, 1024, 2048);

struct cubeloader
{
    enum
    {
        DEFAULT_LIQUID = 1,
        DEFAULT_WALL,
        DEFAULT_FLOOR,
        DEFAULT_CEIL
    };

    enum                              // block types, order matters!
    {
        C_SOLID = 0,                  // entirely solid cube [only specifies wtex]
        C_CORNER,                     // half full corner of a wall
        C_FHF,                        // floor heightfield using neighbour vdelta values
        C_CHF,                        // idem ceiling
        C_SPACE,                      // entirely empty cube
        C_SEMISOLID,                  // generated by mipmapping
        C_MAXTYPE
    };
    
    struct c_sqr
    {
        uchar type;                 // one of the above
        char floor, ceil;           // height, in cubes
        uchar wtex, ftex, ctex;     // wall/floor/ceil texture ids
        uchar vdelta;               // vertex delta, used for heightfield cubes
        uchar utex;                 // upper wall tex id
    };

    struct c_persistent_entity        // map entity
    {
        short x, y, z;              // cube aligned position
        short attr1;
        uchar type;                 // type is one of the above
        uchar attr2, attr3, attr4;
    };

    struct c_header                   // map file format header
    {
        char head[4];               // "CUBE"
        int version;                // any >8bit quantity is little endian
        int headersize;             // sizeof(header)
        int sfactor;                // in bits
        int numents;
        char maptitle[128];
        uchar texlists[3][256];
        int waterlevel;
        int reserved[15];
    };

    c_sqr *world;
    int ssize;
    int x0, x1, y0, y1, z0, z1;
    c_sqr *o[4];
    int lastremip;
    int progress;

    void create_ent(c_persistent_entity &ce)
    {
        if(ce.type>=7) ce.type++;  // grenade ammo
        if(ce.type>=8) ce.type++;  // pistol ammo
        if(ce.type==16) ce.type = ET_MAPMODEL;
        else if(ce.type>=ET_MAPMODEL && ce.type<16) ce.type++;
        if(ce.type>=ET_ENVMAP) ce.type++;
        if(ce.type>=ET_PARTICLES) ce.type++; 
        if(ce.type>=ET_SOUND) ce.type++;
        if(ce.type>=ET_SPOTLIGHT) ce.type++;
        extentity &e = *entities::newentity();
        entities::getents().add(&e);
        e.type = ce.type;
        e.spawned = false;
        e.inoctanode = false;
        e.o = vec(ce.x*4+worldsize/4, ce.y*4+worldsize/4, ce.z*4+worldsize/2);
        e.light.color = vec(1, 1, 1);
        e.light.dir = vec(0, 0, 1);
        e.attr1 = ce.attr1;
        e.attr2 = ce.attr2;
        switch(e.type)
        {
            case ET_MAPMODEL:
                e.o.z += ce.attr3*4;
                e.attr3 = e.attr4 = 0;
                break;
            case ET_LIGHT:
                e.attr1 *= 4;
                if(!ce.attr3 && !ce.attr4) { e.attr3 = e.attr4 = e.attr2; break; }
                // fall through
            default:
                e.attr3 = ce.attr3;
                e.attr4 = ce.attr4;
                break;
        }
        switch(e.type)
        {
            case ET_PLAYERSTART:
            case ET_MAPMODEL:
            case ET_GAMESPECIFIC+12: // teleport
            case ET_GAMESPECIFIC+13: // monster
                e.attr1 = (int(e.attr1)+180)%360;
                break;
        }
        e.attr5 = 0;
    }

    cube &getcube(int x, int y, int z)
    {
        return lookupcube(x*4+worldsize/4, y*4+worldsize/4, z*4+worldsize/2, 4);
    }

    int neighbours(c_sqr &t)
    {
        o[0] = &t;
        o[1] = &t+1;
        o[2] = &t+ssize;
        o[3] = &t+ssize+1;
        int best = 0xFFFF;
        loopi(4) if(o[i]->vdelta<best) best = o[i]->vdelta;
        return best;
    }

    void preprocess_cubes()     // pull up heighfields to where they don't cross cube boundaries
    {
        for(;;)
        {
            bool changed = false;
            loop(x, ssize)
            {
                loop(y, ssize)
                {
                    c_sqr &t = world[x+y*ssize];
                    if(t.type==C_FHF || t.type==C_CHF)
                    {
                        int bottom = (neighbours(t)&(~3))+4;
                        loopj(4) if(o[j]->vdelta>bottom) { o[j]->vdelta = bottom; changed = true; }
                    }
                }
            }
            if(!changed) break;
        }
    }

    int getfloorceil(c_sqr &s, int &floor, int &ceil)
    {
        floor = s.floor;
        ceil = s.ceil;
        int cap = 0;
        switch(s.type)
        {
            case C_SOLID: floor = ceil; break;
            case C_FHF: floor -= (cap = neighbours(s)&(~3))/4; break;
            case C_CHF: ceil  += (cap = neighbours(s)&(~3))/4; break;
        }
        return cap;
    }

    void boundingbox()
    {
        x0 = y0 = ssize;
        x1 = y1 = 0;
        z0 = 128;
        z1 = -128;
        loop(x, ssize) loop(y, ssize)
        {
            c_sqr &t = world[x+y*ssize];
            if(t.type!=C_SOLID)
            {
                if(x<x0) x0 = x;
                if(y<y0) y0 = y;
                if(x>x1) x1 = x;
                if(y>y1) y1 = y;
                int floor, ceil;
                getfloorceil(t, floor, ceil);
                if(floor<z0) z0 = floor;
                if(ceil>z1) z1 = ceil;
            }
        }
    }

    void hf(int x, int y, int z, int side, int dir, int cap)
    {
        cube &c = getcube(x, y, z);
        loopi(2) loopj(2) edgeset(cubeedge(c, 2, i, j), side, dir*(o[(j<<1)+i]->vdelta-cap)*2+side*8);
    }

    bool cornersolid(int z, c_sqr *s) { return s->type==C_SOLID || z<s->floor || z>=s->ceil; }

    void createcorner(cube &c, int lstart, int lend, int rstart, int rend)
    {
        int ledge = edgemake(lstart, lend);
        int redge = edgemake(rstart, rend);
        cubeedge(c, 1, 0, 0) = ledge;
        cubeedge(c, 1, 1, 0) = ledge;
        cubeedge(c, 1, 0, 1) = redge;
        cubeedge(c, 1, 1, 1) = redge;
    }

    void create_cubes()
    {
        preprocess_cubes();
        boundingbox();
        lastremip = allocnodes;
        progress = 0;
        for(int x = x0-1; x<=x1+1; x++) for(int y = y0-1; y<=y1+1; y++)
        {
            c_sqr &s = world[x+y*ssize];
            int floor, ceil, cap = getfloorceil(s, floor, ceil);
            for(int z = z0-1; z<=z1+1; z++)
            {
                cube &c = getcube(x, y, z);
                c.texture[O_LEFT] = c.texture[O_RIGHT] = c.texture[O_BACK] = c.texture[O_FRONT] = s.type!=C_SOLID && z<ceil ? s.wtex : s.utex;
                c.texture[O_BOTTOM] = s.ctex;
                c.texture[O_TOP] = s.ftex;
                if(z>=floor && z<ceil)
                {
                    setfaces(c, F_EMPTY);
                }
                else if(s.type==C_CORNER)
                {
                    c_sqr *ts, *bs, *ls, *rs;
                    bool tc = cornersolid(z, ts = &s-ssize);
                    bool bc = cornersolid(z, bs = &s+ssize);
                    bool lc = cornersolid(z, ls = &s-1);
                    bool rc = cornersolid(z, rs = &s+1);
                    if     (tc && lc && !bc && !rc) createcorner(c, 0, 8, 0, 0);    // TOP LEFT
                    else if(tc && !lc && !bc && rc) createcorner(c, 0, 0, 0, 8);    // TOP RIGHT
                    else if(!tc && lc && bc && !rc) createcorner(c, 0, 8, 8, 8);    // BOT LEFT
                    else if(!tc && !lc && bc && rc) createcorner(c, 8, 8, 0, 8);    // BOT RIGHT
                    else        // fix texture on ground of a corner
                    {
                        if      (ts->floor-1==z && bs->floor-1!=z) { c.texture[O_TOP] = ts->ftex; }
                        else if (ts->floor-1!=z && bs->floor-1==z) { c.texture[O_TOP] = bs->ftex; }
                        if      (ts->ceil==z && bs->ceil!=z)       { c.texture[O_BOTTOM] = ts->ctex; }
                        else if (ts->ceil!=z && bs->ceil==z)       { c.texture[O_BOTTOM] = bs->ctex; }
                    }
                }
            }
            switch(s.type)
            {
                case C_FHF: hf(x, y, floor-1, 1, -1, cap); break;
                case C_CHF: hf(x, y, ceil, 0, 1, cap); break;
            }
            if(importcuberemip && (allocnodes - lastremip) * 8 > importcuberemip * 1024)
            {
                mpremip(true);
                lastremip = allocnodes;
            }
            if((progress++&0x7F)==0)
            {
                float bar = float((y1-y0+2)*(x-x0+1) + y-y0+1) / float((y1-y0+2)*(x1-x0+2));
                defformatstring(text)("creating cubes... %d%%", int(bar*100));
                renderprogress(bar, text);
            }
        }
    }

    void load_cube_world(char *mname)
    {
        int loadingstart = SDL_GetTicks();
        string pakname, cgzname;
        formatstring(pakname)("cube/%s", mname);
        formatstring(cgzname)("%s/%s.cgz", packagesdir, pakname);
        stream *f = opengzfile(path(cgzname), "rb");
        if(!f) { conoutf(CON_ERROR, "could not read cube map %s", cgzname); return; }
        c_header hdr;
        f->read(&hdr, sizeof(c_header)-sizeof(int)*16);
        lilswap(&hdr.version, 4);
        bool mod = false;
        if(strncmp(hdr.head, "CUBE", 4)) 
        { 
            if(!strncmp(hdr.head, "ACMP", 4)) mod = true;
            else
            {
                conoutf(CON_ERROR, "map %s has malformatted header", cgzname); 
                delete f;
                return; 
            }
        }
        else if(hdr.version>5) mod = true;
        if(hdr.version>5 && !mod) { conoutf(CON_ERROR, "map %s requires a newer version of the Cube 1 importer", cgzname); delete f; return; }
        if(!haslocalclients()) game::forceedit("");
        emptymap(12, true, NULL);
        freeocta(worldroot);
        worldroot = newcubes(F_SOLID);
        defformatstring(cs)("importing %s", cgzname);
        renderbackground(cs);
        if(hdr.version>=4)
        {
            f->read(&hdr.waterlevel, sizeof(int)*16);
            lilswap(&hdr.waterlevel, 16);
        }
        else
        {
            hdr.waterlevel = -100000;
        }
        if(mod) f->seek(hdr.numents*sizeof(c_persistent_entity), SEEK_CUR);
        else loopi(hdr.numents)
        {
            c_persistent_entity e;
            f->read(&e, sizeof(c_persistent_entity));
            lilswap(&e.x, 4);
            if(i < MAXENTS) create_ent(e);
        }
        ssize = 1<<hdr.sfactor;
        world = new c_sqr[ssize*ssize];
        c_sqr *t = NULL;
        loopk(ssize*ssize)
        {
            c_sqr *s = &world[k];
            int type = f->getchar();
            switch(type)
            {
                case 255:
                {
                    int n = f->getchar();
                    for(int i = 0; i<n; i++, k++) memcpy(&world[k], t, sizeof(c_sqr));
                    k--;
                    break;
                }
                case 254: // only in MAPVERSION<=2
                {
                    memcpy(s, t, sizeof(c_sqr));
                    f->getchar();
                    f->getchar();
                    break;
                }
                case C_SOLID:
                {
                    s->type = C_SOLID;
                    s->wtex = f->getchar();
                    s->vdelta = f->getchar();
                    if(hdr.version<=2) { f->getchar(); f->getchar(); }
                    s->ftex = DEFAULT_FLOOR;
                    s->ctex = DEFAULT_CEIL;
                    s->utex = s->wtex;
                    s->floor = 0;
                    s->ceil = 16;
                    break;
                }
                default:
                {
                    if(type<0 || type>=C_MAXTYPE)
                    {
                        defformatstring(t)("%d @ %d", type, k);
                        fatal("while reading map: type out of range: %s", t);
                    }
                    s->type = type;
                    s->floor = f->getchar();
                    s->ceil = f->getchar();
                    if(s->floor>=s->ceil) s->floor = s->ceil-1;  // for pre 12_13
                    s->wtex = f->getchar();
                    s->ftex = f->getchar();
                    s->ctex = f->getchar();
                    if(hdr.version<=2) { f->getchar(); f->getchar(); }
                    s->vdelta = f->getchar();
                    s->utex = (hdr.version>=2) ? f->getchar() : s->wtex;
                    if(hdr.version>=5) f->getchar();
                    s->type = type;
                }
            }
            t = s;
        }
        delete f;

        string cfgname;
        formatstring(cfgname)("%s/cube/%s.cfg", packagesdir, mname);
        identflags |= IDF_OVERRIDDEN;
        defformatstring(pkgname)("%s/cube/package.cfg", packagesdir);
        execfile(pkgname);
        execfile(path(cfgname));
        identflags &= ~IDF_OVERRIDDEN;
        create_cubes();
        mpremip(true);
        clearlights();
        allchanged();
        vector<extentity *> &ents = entities::getents();
        loopv(ents) if(ents[i]->type!=ET_LIGHT) dropenttofloor(ents[i]);
        entitiesinoctanodes();
        conoutf("read cube map %s (%.1f seconds)", cgzname, (SDL_GetTicks()-loadingstart)/1000.0f);
        startmap(NULL);
    }
};

void importcube(char *name)
{ 
    if(multiplayer()) return;
    cubeloader().load_cube_world(name); 
}

COMMAND(importcube, "s");
