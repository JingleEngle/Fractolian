#include "FractalModule.hpp"
#ifndef FRACTAL_HEADLESS_TEST
#include <osdialog.h>
#include <fstream>
#include <functional>

namespace {
using F = FractalXY;
float px(float mm) { return mm * SVG_DPI / 25.4f; }
NVGcolor ink() { return nvgRGB(228,239,244); }
NVGcolor muted() { return nvgRGB(177,199,211); }
NVGcolor cyan() { return nvgRGB(100,223,226); }
NVGcolor orange() { return nvgRGB(239,176,117); }
NVGcolor purple() { return nvgRGB(195,176,231); }
NVGcolor gold() { return nvgRGB(222,194,149); }
NVGcolor accent(int i) { const NVGcolor c[]={cyan(),orange(),purple(),gold()}; return c[i&3]; }
NVGcolor controlColor(int i) { const int c[]={0,1,1,0,2,3,0,2,1,3,0,1};return accent(c[i]); }
const int controls[]={F::X_PARAM,F::Y_PARAM,F::ZOOM_PARAM,F::POSITION_PARAM,F::ROUGH_PARAM,F::SMOOTH_PARAM,
    F::TUNE_PARAM,F::OCTAVE_PARAM,F::GLIDE_PARAM,F::LEVEL_PARAM,F::POSITION_SLEW_PARAM,F::ZOOM_SLEW_PARAM};
const int attenuators[]={F::X_ATT,F::Y_ATT,F::ZOOM_ATT,F::POSITION_ATT,F::ROUGH_ATT,F::SMOOTH_ATT,
    F::TUNE_ATT,F::OCTAVE_ATT,F::GLIDE_ATT,F::LEVEL_ATT,F::POSITION_SLEW_ATT,F::ZOOM_SLEW_ATT};
const int inputs[]={F::X_INPUT,F::Y_INPUT,F::ZOOM_INPUT,F::POSITION_INPUT,F::ROUGH_INPUT,F::SMOOTH_INPUT,
    F::PITCH_INPUT,F::OCTAVE_INPUT,F::GLIDE_INPUT,F::LEVEL_INPUT,F::POSITION_SLEW_INPUT,F::ZOOM_SLEW_INPUT};
const char* labels[]={"MORPH X","MORPH Y","ZOOM","POSITION","ROUGHNESS","SMOOTHING",
    "TUNE","OCTAVE","GLIDE","LEVEL","POS SLEW","ZOOM SLEW"};

enum class Typeface { Label, Number };

std::shared_ptr<window::Font> panelFont(Typeface face) {
    if(!APP || !APP->window) return nullptr;
    static const auto label=asset::system("res/fonts/Nunito-Bold.ttf");
    static const auto number=asset::system("res/fonts/ShareTechMono-Regular.ttf");
    auto font=APP->window->loadFont(face==Typeface::Number ? number : label);
    return font ? font : APP->window->uiFont;
}

void textAt(NVGcontext* vg,float x,float y,const std::string& s,float size=10,NVGcolor color=ink(),int align=NVG_ALIGN_CENTER,Typeface face=Typeface::Label) {
    auto font=panelFont(face);
    if(!font)return;
    nvgFontFaceId(vg,font->handle);nvgFontSize(vg,size);
    nvgTextAlign(vg,align|NVG_ALIGN_MIDDLE);nvgFillColor(vg,color);
    nvgText(vg,px(x),px(y),s.c_str(),nullptr);
}
void boxFrame(NVGcontext* vg,float w,float h,NVGcolor border=nvgRGB(79,107,125),NVGcolor fill=nvgRGB(8,18,28)) {
    nvgBeginPath(vg);nvgRoundedRect(vg,.5f,.5f,w-1,h-1,1.3f);nvgFillColor(vg,fill);nvgFill(vg);
    nvgStrokeColor(vg,border);nvgStrokeWidth(vg,.8f);nvgStroke(vg);
}
void recordParam(F* m,int id,float before,const char* name) {
    if(!m || !APP->history || before==m->params[id].getValue())return;
    auto* h=new history::ParamChange;h->name=name;h->moduleId=m->id;h->paramId=id;
    h->oldValue=before;h->newValue=m->params[id].getValue();APP->history->push(h);
}

// Native Rack knob gestures (including Shift fine adjustment and menus),
// with a compact face matching the browser reference.
struct Dial;
struct DialPainter : widget::Widget {Dial* owner=nullptr;void draw(const DrawArgs& a) override;};
struct Dial : app::Knob {
    NVGcolor color=cyan();bool small=false;
    widget::FramebufferWidget* fb=nullptr;DialPainter* painter=nullptr;
    float cachedValue=std::numeric_limits<float>::quiet_NaN();NVGcolor cachedColor{};
    Dial() {
        box.size=mm2px(Vec(10,10));smooth=false;minAngle=-.75*M_PI;maxAngle=.75*M_PI;
        fb=new widget::FramebufferWidget;fb->box.size=box.size;addChild(fb);
        painter=new DialPainter;painter->owner=this;painter->box.size=box.size;fb->addChild(painter);
    }
    float scaled() {
        if(auto* q=getParamQuantity())return q->getScaledValue();
        // Module browser thumbnails have no Module. Show actual default knob
        // positions without constructing an audio engine or source bank.
        if(small)return paramId==F::TUNE_ATT?1.f:.5f;
        switch(paramId) {
            case F::ZOOM_PARAM:return 0;
            case F::ROUGH_PARAM:return .68f;
            case F::SMOOTH_PARAM:return .25f;
            case F::TUNE_PARAM:return 21.f/24;
            case F::OCTAVE_PARAM:return 3.f/8;
            case F::GLIDE_PARAM:return fractal::glideNormalized(.02);
            case F::LEVEL_PARAM:return 42.f/57;
            case F::POSITION_SLEW_PARAM:return .016f;
            case F::ZOOM_SLEW_PARAM:return .035f;
            default:return .5f;
        }
    }
    void step() override {
        float value=scaled();
        if(value!=cachedValue || color.r!=cachedColor.r || color.g!=cachedColor.g || color.b!=cachedColor.b || fb->box.size.x!=box.size.x) {
            cachedValue=value;cachedColor=color;fb->box.size=box.size;painter->box.size=box.size;fb->dirty=true;
        }
        Knob::step();
    }
    void onDragMove(const event::DragMove& e) override {
        const float savedSpeed=speed;
        auto* m=dynamic_cast<F*>(module);
        if(m&&paramId==F::POSITION_PARAM&&m->params[F::POSITION_MODE_PARAM].getValue()==1
                &&!(APP->window->getMods()&GLFW_MOD_SHIFT)){
            const auto bank=m->builder.preview();
            if(bank){
                fractal::live::Settings t;
                t.x=m->liveX.load();t.y=m->liveY.load();
                t.autoLevel=m->params[F::AUTO_LEVEL_PARAM].getValue();
                t.logZoom=m->params[F::ZOOM_ENABLED_PARAM].getValue()>.5?m->params[F::ZOOM_PARAM].getValue():0;
                const double span=std::exp2(-t.logZoom),pos=m->params[paramId].getValue();
                const double direction=horizontal?e.mouseDelta.x:-e.mouseDelta.y;
                int band;double blend;
                fractal::live::selectBand(std::max(1.,m->liveHz.load()*span),APP->engine->getSampleRate(),band,blend);
                const double delta=fractal::clamp(direction*.001, -pos,1-pos);
                const double rate=fractal::live::shapeRate(*bank,t,pos,delta,span,band,blend);
                speed*=std::max(.28,1/std::hypot(1.,std::max(0.,.65*span/rate-12)/8));
            }
        }
        Knob::onDragMove(e);speed=savedSpeed;
    }
    void initParamQuantity() override {
        auto* q=getParamQuantity();bool snapped=q && q->snapEnabled;
        snap=snapped;smooth=false;Knob::initParamQuantity();
        if(q){q->smoothEnabled=false;q->snapEnabled=snapped;}
    }
    void paint(const DrawArgs& a) {
        auto* vg=a.vg;const float r=box.size.x*.5f,c=r;
        const float value=scaled();
        const float theta=minAngle+(maxAngle-minAngle)*value;
        nvgBeginPath(vg);nvgCircle(vg,c,c,r-.6f);nvgFillColor(vg,nvgRGB(21,37,49));nvgFill(vg);
        nvgStrokeWidth(vg,.8f);nvgStrokeColor(vg,small?muted():gold());nvgStroke(vg);
        if(!small) {
            nvgBeginPath(vg);nvgArc(vg,c,c,r-2.5f,minAngle-M_PI_2,theta-M_PI_2,NVG_CW);
            nvgStrokeWidth(vg,1.15f);nvgStrokeColor(vg,color);nvgStroke(vg);
        }
        const float body=small?r-1.4f:r-4.5f;
        nvgBeginPath(vg);nvgCircle(vg,c,c,body);
        nvgFillPaint(vg,nvgRadialGradient(vg,c-body*.4f,c-body*.4f,0,body*1.8f,nvgRGB(92,121,139),nvgRGB(23,42,56)));nvgFill(vg);
        nvgStrokeColor(vg,muted());nvgStrokeWidth(vg,.7f);nvgStroke(vg);
        const float sx=std::sin(theta),sy=-std::cos(theta);
        nvgBeginPath(vg);nvgMoveTo(vg,c+sx*body*.48f,c+sy*body*.48f);nvgLineTo(vg,c+sx*body*.88f,c+sy*body*.88f);
        nvgStrokeWidth(vg,small?1.1f:1.6f);nvgStrokeColor(vg,small?ink():color);nvgStroke(vg);
    }
};
void DialPainter::draw(const DrawArgs& a) {owner->paint(a);Widget::draw(a);}
struct FineDial : Dial {FineDial(){small=true;box.size=mm2px(Vec(6,6));}};
// PortWidget retains native cable interaction and context menus.
struct Jack : app::PortWidget {
    Jack(){box.size=mm2px(Vec(6,6));}
    void draw(const DrawArgs& a) override {
        auto* vg=a.vg;float r=box.size.x*.5f;
        nvgBeginPath(vg);nvgCircle(vg,r,r,r-.6f);nvgFillColor(vg,nvgRGB(45,72,88));nvgFill(vg);
        nvgStrokeColor(vg,nvgRGB(173,204,218));nvgStrokeWidth(vg,1.8f);nvgStroke(vg);
        nvgBeginPath(vg);nvgCircle(vg,r,r,r-3.4f);nvgFillColor(vg,nvgRGB(2,8,14));nvgFill(vg);
        nvgStrokeWidth(vg,.7f);nvgStrokeColor(vg,nvgRGB(12,29,41));nvgStroke(vg);
        PortWidget::draw(a);
    }
};
struct TriggerJack : Jack {TriggerJack(){box.size=mm2px(Vec(5.4,5.4));}};
struct OutputJack : Jack {OutputJack(){box.size=mm2px(Vec(7,7));}};

struct ValueReadout : app::ParamWidget {
    int control=-1,corner=-1;NVGcolor color=cyan();std::string cached;
    double previous=std::numeric_limits<double>::quiet_NaN();
    ValueReadout(){box.size=mm2px(Vec(fractal::panel::READOUT_W,fractal::panel::READOUT_H));}
    void step() override {
        auto* m=dynamic_cast<F*>(module);double value=0;
        if(m) {
            if(corner>=0){uint32_t p=m->pendingSeeds[corner].load();value=p?p:m->exactSeeds[corner].load();}
            else if(control==6)value=m->liveHz.load();
            else if(control==8)value=m->liveGlideMs.load();
            else value=m->params[paramId].getValue();
        } else value=corner>=0?fractal::SeedSet().seeds[corner]:fractal::panel::defaultValue(control);
        if(value!=previous || cached.empty()) {
            previous=value;
            if(corner>=0)cached=m?m->getParamQuantity(paramId)->getDisplayValueString():std::to_string(static_cast<uint32_t>(value));
            else cached=m?m->panelReadout(control):fractal::panel::readout(control,value);
        }
        ParamWidget::step();
    }
    void draw(const DrawArgs& a) override {
        auto* vg=a.vg;boxFrame(vg,box.size.x,box.size.y);
        nvgSave(vg);nvgIntersectScissor(vg,2,1,box.size.x-4,box.size.y-2);
        float size=corner>=0?11.f:11.8f;
        if(auto font=panelFont(Typeface::Number)) {
            nvgFontFaceId(vg,font->handle);nvgFontSize(vg,size);
            float width=nvgTextBounds(vg,0,0,cached.c_str(),nullptr,nullptr);
            if(width>box.size.x-7)size*= (box.size.x-7)/width;
        }
        textAt(vg,box.size.x/px(2),box.size.y/px(2),cached,size,corner>=0?ink():color,NVG_ALIGN_CENTER,Typeface::Number);
        nvgRestore(vg);Widget::draw(a);
    }
    void onButton(const event::Button& e) override {
        if(module && e.button==GLFW_MOUSE_BUTTON_LEFT && e.action==GLFW_PRESS) {
            createContextMenu();e.consume(this);return;
        }
        ParamWidget::onButton(e);
    }
};
struct TextSwitch : app::Switch {
    std::string label;NVGcolor color=ink();bool checkbox=false,compare=false;
    TextSwitch(){box.size=mm2px(Vec(12,6));}
    void draw(const DrawArgs& a) override {
        auto* q=getParamQuantity();bool on=q?q->getValue()>.5f:(paramId==F::AUTO_LEVEL_PARAM||paramId==F::ZOOM_ENABLED_PARAM);
        bool filled=compare?!on:(momentary&&on);
        auto* vg=a.vg;
        boxFrame(vg,box.size.x,box.size.y,compare?orange():nvgRGB(79,107,125),filled?color:nvgRGB(17,31,44));
        if(checkbox) {
            nvgBeginPath(vg);nvgRect(vg,3,box.size.y*.5f-3.5f,7,7);nvgFillColor(vg,on?cyan():nvgRGB(8,18,28));nvgFill(vg);
            nvgStrokeWidth(vg,.7f);nvgStrokeColor(vg,muted());nvgStroke(vg);
            if(on){nvgBeginPath(vg);nvgMoveTo(vg,4,box.size.y*.5f);nvgLineTo(vg,6,box.size.y*.5f+2);nvgLineTo(vg,9,box.size.y*.5f-2);nvgStrokeColor(vg,nvgRGB(8,18,28));nvgStroke(vg);}
            textAt(vg,4.3f,box.size.y/px(2),label,9.1f,ink(),NVG_ALIGN_LEFT);
        } else if(paramId>=F::NEW_A && paramId<=F::NEW_D) {
            const float cx=box.size.x*.5f,cy=box.size.y*.52f,r=box.size.x*.25f;
            const auto pen=filled?nvgRGB(8,18,28):color;
            nvgBeginPath(vg);nvgArc(vg,cx,cy,r,-1.48f,3.95f,NVG_CW);
            nvgStrokeColor(vg,pen);nvgStrokeWidth(vg,1.3f);nvgStroke(vg);
            nvgBeginPath(vg);nvgMoveTo(vg,cx-1.7f,cy-r);
            nvgLineTo(vg,cx+1.2f,cy-r-1.9f);nvgLineTo(vg,cx+1.2f,cy-r+1.9f);
            nvgClosePath(vg);nvgFillColor(vg,pen);nvgFill(vg);
        } else textAt(vg,box.size.x/px(2),box.size.y/px(2),label,9.8f,filled?nvgRGB(8,18,28):color);
        Widget::draw(a);
    }
};
struct ActionButton : widget::OpaqueWidget {
    std::string label;std::function<void()> action;
    void draw(const DrawArgs& a) override {boxFrame(a.vg,box.size.x,box.size.y);textAt(a.vg,box.size.x/px(2),box.size.y/px(2),label,9.1f);Widget::draw(a);}
    void onButton(const event::Button& e) override {
        if(e.button==GLFW_MOUSE_BUTTON_LEFT&&e.action==GLFW_PRESS){if(action)action();e.consume(this);return;}
        OpaqueWidget::onButton(e);
    }
};
void selectAlgorithmAction(F* m,int corner,int algorithm) {
    if(!m)return;
    int id=F::ALGO_A+corner;float old=m->params[id].getValue();
    if(m->selectAlgorithm(corner,algorithm))recordParam(m,id,old,"choose fractal algorithm");
}
void appendAlgorithmOptions(ui::Menu* menu,F* m,int corner) {
    for(int i=0;i<fractal::ALGORITHMS;++i)menu->addChild(createCheckMenuItem(fractal::algorithmName(i),"",
        [m,corner,i]{return fractal::panel::algorithmIndex(m->params[F::ALGO_A+corner].getValue())==i;},
        [m,corner,i]{selectAlgorithmAction(m,corner,i);}));
}
struct AlgorithmPicker : app::ParamWidget {
    int corner=0;
    AlgorithmPicker(){box.size=mm2px(Vec(18.8,6.2));}
    void draw(const DrawArgs& a) override {
        auto* q=getParamQuantity();int choice=q?fractal::panel::algorithmIndex(q->getValue()):fractal::SeedSet().algorithms[corner];
        auto* vg=a.vg;boxFrame(vg,box.size.x,box.size.y);
        textAt(vg,1.1,3.1,fractal::algorithmName(choice),9.4f,accent(corner),NVG_ALIGN_LEFT);
        nvgBeginPath(vg);nvgMoveTo(vg,box.size.x-7,box.size.y*.5f-1);nvgLineTo(vg,box.size.x-3,box.size.y*.5f-1);nvgLineTo(vg,box.size.x-5,box.size.y*.5f+2);nvgClosePath(vg);nvgFillColor(vg,accent(corner));nvgFill(vg);
        Widget::draw(a);
    }
    void onButton(const event::Button& e) override {
        if(module&&e.button==GLFW_MOUSE_BUTTON_LEFT&&e.action==GLFW_PRESS){auto* menu=createMenu();appendAlgorithmOptions(menu,dynamic_cast<F*>(module),corner);e.consume(this);return;}
        ParamWidget::onButton(e);
    }
    void appendContextMenu(ui::Menu* menu) override {auto* m=dynamic_cast<F*>(module);if(m){menu->addChild(new MenuSeparator);appendAlgorithmOptions(menu,m,corner);}}
};

struct XYPad : widget::OpaqueWidget {
    F* module=nullptr;float oldX=.5f,oldY=.5f;bool dragging=false;
    static float inset() { return px(1.8f); }
    Vec travel() const { return box.size.minus(Vec(2*inset(),2*inset())); }
    void set(double x,double y) {
        if(!module)return;
        module->getParamQuantity(F::X_PARAM)->setValue(fractal::safe(x,0,1,.5));
        module->getParamQuantity(F::Y_PARAM)->setValue(fractal::safe(y,0,1,.5));
    }
    void draw(const DrawArgs& a) override {
        auto* vg=a.vg;boxFrame(vg,box.size.x,box.size.y,nvgRGB(92,125,142));
        nvgBeginPath(vg);
        for(int i=1;i<4;++i){float x=box.size.x*i/4,y=box.size.y*i/4;nvgMoveTo(vg,x,0);nvgLineTo(vg,x,box.size.y);nvgMoveTo(vg,0,y);nvgLineTo(vg,box.size.x,y);}
        nvgStrokeColor(vg,nvgRGB(40,60,77));nvgStrokeWidth(vg,.7f);nvgStroke(vg);
        nvgBeginPath(vg);nvgMoveTo(vg,box.size.x*.5f,px(3));nvgLineTo(vg,box.size.x-px(6),box.size.y*.5f);nvgLineTo(vg,box.size.x*.5f,box.size.y-px(3));nvgLineTo(vg,px(6),box.size.y*.5f);nvgClosePath(vg);nvgStrokeColor(vg,nvgRGBA(185,146,79,100));nvgStroke(vg);
        float x=module?module->params[F::X_PARAM].getValue():.5f,y=module?module->params[F::Y_PARAM].getValue():.5f;
        float lx=module?module->liveX.load():x,ly=module?module->liveY.load():y;
        auto w=fractal::weights(lx,ly);
        textAt(vg,1,2.5,string::f("A %.0f%%",w[0]*100),9.2f,cyan(),NVG_ALIGN_LEFT);
        textAt(vg,box.size.x/px(1)-1,2.5,string::f("B %.0f%%",w[1]*100),9.2f,orange(),NVG_ALIGN_RIGHT);
        textAt(vg,1,box.size.y/px(1)-2.5,string::f("C %.0f%%",w[2]*100),9.2f,cyan(),NVG_ALIGN_LEFT);
        textAt(vg,box.size.x/px(1)-1,box.size.y/px(1)-2.5,string::f("D %.0f%%",w[3]*100),9.2f,orange(),NVG_ALIGN_RIGHT);
        nvgBeginPath(vg);nvgMoveTo(vg,(inset()+lx*travel().x),0);nvgLineTo(vg,(inset()+lx*travel().x),box.size.y);nvgMoveTo(vg,0,(inset()+ly*travel().y));nvgLineTo(vg,box.size.x,(inset()+ly*travel().y));nvgStrokeColor(vg,nvgRGBA(108,168,183,110));nvgStroke(vg);
        float cx=inset()+x*travel().x,cy=inset()+y*travel().y;
        nvgBeginPath(vg);nvgCircle(vg,cx,cy,4.4f);nvgFillColor(vg,nvgRGB(14,35,44));nvgFill(vg);nvgStrokeColor(vg,orange());nvgStrokeWidth(vg,1);nvgStroke(vg);
        nvgBeginPath(vg);nvgCircle(vg,(inset()+lx*travel().x),(inset()+ly*travel().y),1.5f);nvgFillColor(vg,cyan());nvgFill(vg);
        Widget::draw(a);
    }
    void onButton(const event::Button& e) override {
        if(module&&e.button==GLFW_MOUSE_BUTTON_LEFT&&e.action==GLFW_PRESS){oldX=module->params[F::X_PARAM].getValue();oldY=module->params[F::Y_PARAM].getValue();if(!(e.mods&GLFW_MOD_SHIFT))set((e.pos.x-inset())/travel().x,(e.pos.y-inset())/travel().y);dragging=true;e.consume(this);return;}
        OpaqueWidget::onButton(e);
    }
    void onDragMove(const event::DragMove& e) override {
        if(!module||!dragging)return;
        bool fine=(APP->window->getMods()&GLFW_MOD_SHIFT)!=0;double z=std::max(.01f,getAbsoluteZoom());
        set(module->params[F::X_PARAM].getValue()+e.mouseDelta.x*(fine?.00001:1/(travel().x*z)),module->params[F::Y_PARAM].getValue()+e.mouseDelta.y*(fine?.00001:1/(travel().y*z)));
    }
    void onDragEnd(const event::DragEnd& e) override {
        if(module&&dragging){recordParam(module,F::X_PARAM,oldX,"move fractal X");recordParam(module,F::Y_PARAM,oldY,"move fractal Y");}dragging=false;OpaqueWidget::onDragEnd(e);
    }
};

// Offline-generated default traces for null-module browser thumbnails.
// No source generation, allocation, or DSP work is needed to draw a thumbnail.

const std::array<float,192> defaultSource{{
-4.275313318e-01f, -4.360010028e-01f, -4.425318837e-01f, -4.275490046e-01f, -4.258837104e-01f, -3.860068619e-01f,
-3.787151575e-01f, -3.621701896e-01f, -3.268957138e-01f, -3.378274143e-01f, -3.526048064e-01f, -3.473995328e-01f,
-3.115006685e-01f, -2.992730141e-01f, -2.836592495e-01f, -2.709863484e-01f, -2.344080955e-01f, -2.562108636e-01f,
-2.213997096e-01f, -1.695454717e-01f, -1.517316997e-01f, -1.386929452e-01f, -1.440195739e-01f, -7.179831713e-02f,
-3.927029669e-02f, -8.513394743e-03f, -3.084798157e-02f, -6.528759003e-02f, -2.239304595e-02f, -3.758589551e-02f,
-3.673416749e-02f, -6.335995346e-02f, -6.003313512e-02f, -8.868205547e-02f, -7.756028324e-02f, -3.532867506e-02f,
-4.896562546e-02f, -4.776643962e-02f, -4.320549965e-02f, -4.395155609e-02f, -1.845251583e-02f, -3.495849669e-02f,
-8.179078810e-03f, -2.319636568e-02f, -2.276027389e-02f, -2.918717451e-02f, -1.024771295e-02f, -3.231874108e-02f,
-3.255113214e-02f, -2.415267564e-02f, -2.176985703e-02f, 3.038091958e-02f, 1.217284054e-02f, 1.289047301e-02f,
2.598873712e-02f, 2.534881793e-02f, 2.034169808e-02f, 5.559616908e-02f, 7.753841579e-02f, 3.413058072e-02f,
4.255792126e-02f, 6.582604349e-02f, 1.203003600e-01f, 1.027536914e-01f, 1.362378746e-01f, 1.457790285e-01f,
1.568123698e-01f, 1.418767571e-01f, 1.600566655e-01f, 1.903894246e-01f, 2.194140404e-01f, 2.504684925e-01f,
2.268122882e-01f, 2.565518618e-01f, 3.147627413e-01f, 3.148683012e-01f, 3.453872502e-01f, 3.284483254e-01f,
3.206761777e-01f, 3.490706682e-01f, 3.692409992e-01f, 3.489114344e-01f, 3.383028209e-01f, 3.378648162e-01f,
2.930807173e-01f, 2.878894210e-01f, 3.581274450e-01f, 3.612287343e-01f, 3.568522930e-01f, 3.462567925e-01f,
3.789255917e-01f, 3.079692721e-01f, 3.252998888e-01f, 3.532088697e-01f, 3.872642219e-01f, 3.967931867e-01f,
4.239183962e-01f, 4.413428307e-01f, 4.216172695e-01f, 3.828546405e-01f, 3.696064949e-01f, 3.728098273e-01f,
3.463155627e-01f, 2.956709862e-01f, 3.301778734e-01f, 3.047529757e-01f, 2.473416328e-01f, 2.336871028e-01f,
1.981176883e-01f, 2.101712227e-01f, 2.534064949e-01f, 2.501369715e-01f, 2.090006620e-01f, 2.185831815e-01f,
2.031214237e-01f, 2.121506035e-01f, 1.708323509e-01f, 1.453379691e-01f, 1.005990431e-01f, 1.216684133e-01f,
1.394015700e-01f, 1.178268641e-01f, 1.213872284e-01f, 1.257263869e-01f, 7.552523166e-02f, 6.732328236e-02f,
8.533119410e-02f, 5.988591164e-02f, 7.585301995e-02f, 8.600635827e-02f, 7.763443887e-02f, 8.625994623e-02f,
1.014039740e-01f, 9.374037385e-02f, 1.053389087e-01f, 8.728778362e-02f, 9.195324779e-02f, 5.291887373e-02f,
8.717585355e-02f, 1.048560739e-01f, 1.069578230e-01f, 7.513446361e-02f, 9.184157848e-02f, 4.877642542e-02f,
3.621363640e-02f, 3.182769567e-02f, 2.350899018e-02f, -2.301056311e-02f, 1.010981039e-03f, -2.755128033e-02f,
-4.764885083e-02f, -6.089800224e-02f, -1.047777310e-01f, -1.613136232e-01f, -1.481604874e-01f, -1.685117483e-01f,
-1.839376092e-01f, -1.741347760e-01f, -1.422796845e-01f, -1.639948934e-01f, -1.508269012e-01f, -1.552457958e-01f,
-1.586608142e-01f, -1.617338508e-01f, -1.723107100e-01f, -2.004922181e-01f, -2.337193936e-01f, -2.247232050e-01f,
-1.999806166e-01f, -2.145826668e-01f, -2.158959806e-01f, -2.237066031e-01f, -2.421824634e-01f, -2.382588834e-01f,
-2.524925470e-01f, -2.890311778e-01f, -3.432529569e-01f, -3.002850711e-01f, -2.953684628e-01f, -2.799985707e-01f,
-3.496677577e-01f, -3.665787578e-01f, -3.858483732e-01f, -3.810203075e-01f, -4.201359749e-01f, -4.193373322e-01f,
-4.614795744e-01f, -4.433715045e-01f, -4.694336653e-01f, -4.750298262e-01f, -4.198408723e-01f, -4.275313318e-01f
}};
const std::array<float,192> defaultShape{{
-7.816644311e-01f, -8.122673035e-01f, -7.851802707e-01f, -6.970969439e-01f, -6.029157043e-01f, -6.427894235e-01f,
-5.733309388e-01f, -5.243955851e-01f, -4.258350134e-01f, -3.945669234e-01f, -2.752999663e-01f, -2.678038180e-01f,
-7.329627126e-02f, -5.781348050e-02f, -4.122693092e-02f, -6.825010478e-02f, -1.141392514e-01f, -1.399711818e-01f,
-8.887895197e-02f, -8.271514624e-02f, -3.538996354e-02f, -1.303998847e-02f, -4.615396261e-02f, -1.443053596e-02f,
-5.582179502e-02f, -3.808384761e-02f, 1.853566803e-02f, 4.270804301e-02f, 3.936651722e-02f, 1.387778074e-01f,
7.543779910e-02f, 2.171431184e-01f, 2.500369549e-01f, 2.839883268e-01f, 2.953682542e-01f, 4.029623866e-01f,
4.169047475e-01f, 5.710109472e-01f, 6.348614097e-01f, 5.906444192e-01f, 6.746752262e-01f, 6.203684211e-01f,
5.376263261e-01f, 6.536607742e-01f, 6.525290608e-01f, 7.009795308e-01f, 5.959064960e-01f, 7.085679770e-01f,
7.749820948e-01f, 7.754884958e-01f, 6.817501187e-01f, 6.357225776e-01f, 5.966175199e-01f, 4.565307200e-01f,
3.677729070e-01f, 4.647543728e-01f, 3.822383583e-01f, 3.717938960e-01f, 3.150436580e-01f, 1.788678169e-01f,
2.549823225e-01f, 2.203993648e-01f, 1.395252496e-01f, 1.555500776e-01f, 1.425473988e-01f, 1.376567781e-01f,
1.900178939e-01f, 1.977320015e-01f, 1.718278080e-01f, 1.605185270e-01f, 1.967270821e-01f, 1.680818647e-01f,
6.945019215e-02f, 3.365445882e-02f, -3.683760995e-03f, -8.377978206e-02f, -1.916989237e-01f, -2.709761262e-01f,
-3.386322856e-01f, -2.641983330e-01f, -2.758079469e-01f, -2.852430642e-01f, -3.151219785e-01f, -4.253450930e-01f,
-3.669513762e-01f, -4.005358517e-01f, -4.498459399e-01f, -4.643744528e-01f, -6.334916949e-01f, -5.428948998e-01f,
-6.466681957e-01f, -7.099415660e-01f, -7.708550692e-01f, -8.497897983e-01f, -8.661180735e-01f, -7.784579992e-01f,
-8.002467752e-01f, -7.816206813e-01f, -7.113902569e-01f, -6.611638069e-01f, -6.266435385e-01f, -6.360878348e-01f,
-5.479652882e-01f, -4.941923916e-01f, -4.608420432e-01f, -3.138800859e-01f, -2.574298084e-01f, -1.346020550e-01f,
-2.205418050e-02f, -1.189045534e-01f, -7.172492146e-02f, -1.144035459e-01f, -1.657267660e-01f, -6.315192580e-02f,
-8.866272122e-02f, -7.450590283e-02f, -5.967823789e-02f, -4.102145135e-02f, -5.412404984e-02f, -5.135539547e-02f,
-4.406806827e-02f, 5.560049787e-02f, 2.567666396e-02f, 3.765923157e-02f, 1.036487669e-01f, 5.662697926e-02f,
1.228944361e-01f, 1.928630471e-01f, 2.713391185e-01f, 2.607888579e-01f, 3.532472551e-01f, 4.523731768e-01f,
4.666109085e-01f, 5.709876418e-01f, 6.022794843e-01f, 6.309795380e-01f, 6.440088749e-01f, 6.183258295e-01f,
5.268371701e-01f, 6.678357124e-01f, 6.424581409e-01f, 5.632207990e-01f, 6.507889032e-01f, 7.267028689e-01f,
8.128683567e-01f, 7.038126588e-01f, 6.842505336e-01f, 5.416876078e-01f, 5.458188057e-01f, 4.270946681e-01f,
3.923738301e-01f, 4.564259052e-01f, 4.027699530e-01f, 3.850342631e-01f, 2.708524466e-01f, 2.265966237e-01f,
2.149040550e-01f, 2.258059978e-01f, 1.239793301e-01f, 1.163452491e-01f, 1.557079852e-01f, 1.556317955e-01f,
1.613233536e-01f, 1.614679247e-01f, 8.699381351e-02f, 1.907815188e-01f, 1.392891854e-01f, 8.667867631e-02f,
5.503257364e-02f, -4.318517819e-02f, -5.354335904e-02f, -1.101179495e-01f, -2.979902923e-01f, -3.140256703e-01f,
-3.209804595e-01f, -3.031484783e-01f, -2.860187292e-01f, -2.958770394e-01f, -3.702071905e-01f, -4.115402102e-01f,
-4.000768960e-01f, -4.102507234e-01f, -4.415121973e-01f, -5.346580148e-01f, -5.457118750e-01f, -5.119074583e-01f,
-6.672415137e-01f, -7.007082701e-01f, -7.684545517e-01f, -8.076342940e-01f, -8.700000048e-01f, -7.816644311e-01f
}};

struct ScopePair;
struct ScopePainter : widget::Widget {ScopePair* owner=nullptr;void draw(const DrawArgs& a) override;};
struct ScopePair : widget::OpaqueWidget {
    static constexpr int POINTS=192;
    F* module=nullptr;bool dragging=false;float oldPosition=0;
    widget::FramebufferWidget* fb=nullptr;
    std::shared_ptr<const fractal::live::Bank> bank;
    uint64_t build=std::numeric_limits<uint64_t>::max();
    std::array<double,4> state{{.5,.5,.5,0}};
    std::array<float,POINTS> source=defaultSource;
    OutputScope::Frame output;
    int voltsPerHalf=1;
    double lastUpdate=-1;
    static float sourceWidth() { return px(30.3f); }
    ScopePair() {
        box.size=mm2px(Vec(82.24,21.3));fb=new widget::FramebufferWidget;fb->box.size=box.size;addChild(fb);
        auto* p=new ScopePainter;p->owner=this;p->box.size=box.size;fb->addChild(p);
    }
    void draw(const DrawArgs& a) override {
        if(module) module->scope.drawn.store(true, std::memory_order_relaxed);
        OpaqueWidget::draw(a);
    }
    void step() override {
        const double now=system::getTime();
        if(module && now-lastUpdate>=1./30.) {
            lastUpdate=now;
            if(module->scope.read(output)) {
                float peak=0;
                for(int i=0;i<output.count;++i) peak=std::max(peak,std::abs(output.volts[i]));
                voltsPerHalf=peak<=1 ? 1 : (peak<=2 ? 2 : 5);
                fb->dirty=true;
            }
            const auto count=module->builder.builds.load(std::memory_order_relaxed);
            const bool changed=count!=build;
            if(changed){build=count;bank=module->builder.preview();}
            std::array<double,4> values{{module->liveX.load(),module->liveY.load(),
                module->livePosition.load(),module->liveLogZoom.load()}};
            if(changed || values!=state) {
                const bool morph=changed || values[0]!=state[0] || values[1]!=state[1];
                state=values;
                if(morph && bank && bank->valid()) {
                    const auto weights=fractal::weights(state[0],state[1]);
                    for(int i=0;i<POINTS;++i) {
                        const auto point=fractal::live::detail::coordinate(double(i)/(POINTS-1));
                        double mix=0;
                        for(int j=0;j<4;++j) {
                            const auto& v=bank->corners[j]->source;
                            mix+=(v[point.index]+(v[point.index+1]-v[point.index])*point.fraction)*weights[j];
                        }
                        source[i]=mix;
                    }
                }
                fb->dirty=true;
            }
        }
        OpaqueWidget::step();
    }
    void onButton(const event::Button& e) override {
        if(module&&e.button==GLFW_MOUSE_BUTTON_LEFT&&e.action==GLFW_PRESS&&e.pos.x<sourceWidth()&&e.pos.y>px(4.5)) {
            oldPosition=module->params[F::POSITION_PARAM].getValue();dragging=true;
            if(!(e.mods&GLFW_MOD_SHIFT))module->getParamQuantity(F::POSITION_PARAM)->setValue(fractal::safe(e.pos.x/sourceWidth(),0,1,.5));
            e.consume(this);return;
        }
        OpaqueWidget::onButton(e);
    }
    void onDragMove(const event::DragMove& e) override {
        if(!dragging||!module)return;
        bool fine=(APP->window->getMods()&GLFW_MOD_SHIFT)!=0;
        double dx=e.mouseDelta.x*(fine?.00001:1/(sourceWidth()*std::max(.01f,getAbsoluteZoom())));
        module->getParamQuantity(F::POSITION_PARAM)->setValue(fractal::safe(module->params[F::POSITION_PARAM].getValue()+dx,0,1,.5));
    }
    void onDragEnd(const event::DragEnd& e) override {if(dragging&&module)recordParam(module,F::POSITION_PARAM,oldPosition,"move fractal position");dragging=false;OpaqueWidget::onDragEnd(e);}
};
void ScopePainter::draw(const DrawArgs& a) {
    auto* vg=a.vg;float gap=px(1.2f),h=box.size.y,top=px(4.4f),waveH=h-top-1;
    for(int k=0;k<2;++k) {
        const float left=k?ScopePair::sourceWidth()+gap:0;
        const float w=k?box.size.x-left:ScopePair::sourceWidth();
        nvgSave(vg);nvgTranslate(vg,left,0);boxFrame(vg,w,h);
        textAt(vg,1,2.3,k?"OUTPUT":"SOURCE",9.1f,muted(),NVG_ALIGN_LEFT);
        const double z=owner->state[3]>=0?std::exp2(owner->state[3]):1;
        textAt(vg,w/px(1)-1,2.3,k?(owner->module?string::f("CH 1   ±%d V",owner->voltsPerHalf):"CH 1"):fractal::panel::fixed(z,2)+"x",8.3f,orange(),NVG_ALIGN_RIGHT);
        nvgIntersectScissor(vg,1,top,w-2,waveH);
        nvgBeginPath(vg);nvgMoveTo(vg,0,top+waveH*.5f);nvgLineTo(vg,w,top+waveH*.5f);nvgStrokeColor(vg,nvgRGB(35,53,68));nvgStrokeWidth(vg,.6f);nvgStroke(vg);
        if(k==0) {
            double span=1/z,pos=owner->state[2]>=0?owner->state[2]:.5,start=pos-span*.5;
            for(int t=-1;t<=1;++t){double l=std::max(0.,start+t),r=std::min(1.,start+span+t);if(r<=l)continue;nvgBeginPath(vg);nvgRect(vg,l*w,top,(r-l)*w,waveH);nvgFillColor(vg,nvgRGBA(100,223,226,18));nvgFill(vg);nvgStrokeColor(vg,nvgRGBA(239,176,117,150));nvgStroke(vg);}
        }
        const bool live=k && owner->module;
        const auto& values=live?owner->output.volts:(k?defaultShape:owner->source);
        const int points=live?owner->output.count:ScopePair::POINTS;
        const float scale=live?.49f/owner->voltsPerHalf:.42f;
        nvgBeginPath(vg);
        for(int i=0;i<points;++i){float x=float(i)*w/(points-1),y=top+waveH*.5f-values[i]*waveH*scale;if(i)nvgLineTo(vg,x,y);else nvgMoveTo(vg,x,y);}
        nvgStrokeColor(vg,k?cyan():nvgRGB(157,176,191));nvgStrokeWidth(vg,k?1.25f:.95f);nvgStroke(vg);nvgRestore(vg);
    }
    Widget::draw(a);
}

void exportTones(F* m) {
    char* path=osdialog_file(OSDIALOG_SAVE,nullptr,"Fractolian-tones.json",nullptr);if(!path)return;
    json_t* root=m->dataToJson();int result=json_dump_file(root,path,JSON_INDENT(2)|JSON_ENSURE_ASCII);json_decref(root);std::free(path);
    if(result)osdialog_message(OSDIALOG_WARNING,OSDIALOG_OK,"Could not write the tone file. Choose a writable folder.");
}
void importTones(F* m) {
    char* path=osdialog_file(OSDIALOG_OPEN,nullptr,nullptr,nullptr);if(!path)return;
    std::ifstream file(path,std::ios::binary|std::ios::ate);std::free(path);
    if(!file||file.tellg()<0||file.tellg()>1024*1024){osdialog_message(OSDIALOG_WARNING,OSDIALOG_OK,"Choose a Fractolian tone JSON file smaller than 1 MB.");return;}
    file.seekg(0);std::ostringstream stream;stream<<file.rdbuf();std::string contents=stream.str();json_error_t error;
    json_t* root=json_loadb(contents.data(),contents.size(),0,&error);
    if(!root){osdialog_message(OSDIALOG_WARNING,OSDIALOG_OK,"This file is not valid JSON.");return;}
    bool ok=m->queueImport(root);json_decref(root);
    if(!ok)osdialog_message(OSDIALOG_WARNING,OSDIALOG_OK,"This file is not a supported Fractolian tone library, contains invalid data, or another import is pending.");
}
struct BankNameField : ui::TextField {
    F* module=nullptr;int index=0;
    void onChange(const event::Change& e) override {TextField::onChange(e);if(module)module->setBankName(index,text);}
};
void addBankOptions(ui::Menu* menu,F* m) {
    for(int i=0;i<16;++i) {
        std::string name=m->bankName(i);bool occupied=m->memories.occupied[i].load();
        std::string label=string::f("%02d  ",i+1)+(name.empty()?(occupied?"Saved tone":"Empty"):name);
        menu->addChild(createCheckMenuItem(label,"",[m,i]{return m->selectedSlot()==i;},[m,i]{float old=m->params[F::SLOT_PARAM].getValue();m->getParamQuantity(F::SLOT_PARAM)->setValue(i);recordParam(m,F::SLOT_PARAM,old,"select fractal bank");}));
    }
    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuItem("Export all tones…","",[m]{exportTones(m);}));
    menu->addChild(createMenuItem("Import tone file…","",[m]{importTones(m);}));
}
struct BankPicker : app::ParamWidget {
    std::string cached="01 · Empty";int ticks=0;
    BankPicker(){box.size=mm2px(Vec(39,7));}
    void step() override {
        if(auto* m=dynamic_cast<F*>(module))if((ticks++&3)==0){int slot=m->selectedSlot();std::string name=m->bankName(slot);bool saved=m->memories.occupied[slot].load();cached=string::f("%02d · ",slot+1)+(name.empty()?(saved?"Saved tone":"Empty"):name);}
        ParamWidget::step();
    }
    void draw(const DrawArgs& a) override {
        auto* vg=a.vg;boxFrame(vg,box.size.x,box.size.y);nvgSave(vg);nvgIntersectScissor(vg,3,1,box.size.x-16,box.size.y-2);
        textAt(vg,1.4,3.5,cached,10,ink(),NVG_ALIGN_LEFT);nvgRestore(vg);
        nvgBeginPath(vg);nvgMoveTo(vg,box.size.x-10,8);nvgLineTo(vg,box.size.x-5,8);nvgLineTo(vg,box.size.x-7.5f,11);nvgClosePath(vg);nvgFillColor(vg,muted());nvgFill(vg);Widget::draw(a);
    }
    void onButton(const event::Button& e) override {
        if(module&&e.button==GLFW_MOUSE_BUTTON_LEFT&&e.action==GLFW_PRESS){auto* menu=createMenu();addBankOptions(menu,dynamic_cast<F*>(module));e.consume(this);return;}ParamWidget::onButton(e);
    }
};

struct Face : widget::Widget {
    F* module=nullptr;
    std::array<float,12> lastAtt;std::array<std::string,12> attText;
    Face(){lastAtt.fill(std::numeric_limits<float>::quiet_NaN());}
    void step() override {
        for(int i=0;i<12;++i){float value=module?module->params[attenuators[i]].getValue():(i==6?1:0);if(value!=lastAtt[i]){lastAtt[i]=value;attText[i]=string::f("%+.2f",value);}}
        Widget::step();
    }
    void draw(const DrawArgs& a) override {
        auto* vg=a.vg;
        textAt(vg,4,4.6,"JINGLEENGLE",10,orange(),NVG_ALIGN_LEFT);
        textAt(vg,138.0,4.6,"28 HP",9.8f,muted(),NVG_ALIGN_RIGHT);
        textAt(vg,2,11.5,"X / Y",10,muted(),NVG_ALIGN_LEFT);
        for(int i=0;i<4;++i){float left=fractal::panel::seedLeft(i);textAt(vg,left+2,13.5,std::string(1,char('A'+i)),11,accent(i));textAt(vg,left+12.05,13.5,"TRG",7.7f,muted());}
        for(int i=0;i<12;++i) {
            float x=fractal::panel::controlX(i);
            textAt(vg,x,fractal::panel::titleY(i),labels[i],11.3f,ink());
            textAt(vg,x-6,fractal::panel::legendY(i),attText[i],10.3f,muted(),NVG_ALIGN_CENTER,Typeface::Number);
            textAt(vg,x+6,fractal::panel::legendY(i),i==6?"1V/OCT":"CV",9.5f,ink());
        }
        textAt(vg,2,122,"BANK",9.5f,muted(),NVG_ALIGN_LEFT);
        int channels=module?module->liveChannels.load():1;
        textAt(vg,111,122,string::f("%d ch",channels),9.2f,muted());
        textAt(vg,137,122,"OUT",9.2f,nvgRGB(7,27,37));
        Widget::draw(a);
    }
};
struct FractalXYWidget : ModuleWidget {
    ScopePair* scopes=nullptr;
    explicit FractalXYWidget(F* m) {
        setModule(m);setPanel(createPanel(asset::plugin(pluginInstance,"res/FractalXY.svg")));
        auto* face=new Face;face->module=m;face->box.size=box.size;addChild(face);
        auto* pad=new XYPad;pad->module=m;pad->box.pos=mm2px(Vec(2,15.5));pad->box.size=mm2px(Vec(54,38));addChild(pad);
        auto* center=new ActionButton;center->label="Center";center->box.pos=mm2px(Vec(11,9.4));center->box.size=mm2px(Vec(11,5.3));
        center->action=[m]{if(m){float x=m->params[F::X_PARAM].getValue(),y=m->params[F::Y_PARAM].getValue();m->getParamQuantity(F::X_PARAM)->setValue(.5);m->getParamQuantity(F::Y_PARAM)->setValue(.5);recordParam(m,F::X_PARAM,x,"center fractal X");recordParam(m,F::Y_PARAM,y,"center fractal Y");}};addChild(center);
        auto* level=createParam<TextSwitch>(mm2px(Vec(23,9.4)),m,F::AUTO_LEVEL_PARAM);level->box.size=mm2px(Vec(19,5.3));level->checkbox=true;level->label="Auto level";addParam(level);
        auto* compare=createParam<TextSwitch>(mm2px(Vec(43,9.4)),m,F::ZOOM_ENABLED_PARAM);compare->box.size=mm2px(Vec(13,5.3));compare->compare=true;compare->label="A/B 1x";compare->color=orange();addParam(compare);
        for(int i=0;i<4;++i) {
            float left=fractal::panel::seedLeft(i);
            auto* trigger=createParam<TextSwitch>(mm2px(Vec(left+4,10.6)),m,F::NEW_A+i);trigger->box.size=mm2px(Vec(5.3,5.7));trigger->label="↻";trigger->color=accent(i);trigger->momentary=true;addParam(trigger);
            addInput(createInputCentered<TriggerJack>(mm2px(Vec(left+17.35,13.5)),m,F::NEW_A_INPUT+i));
            auto* value=createParam<ValueReadout>(mm2px(Vec(left+.55,18)),m,F::SEED_A+i);value->corner=i;value->box.size=mm2px(Vec(18.8,5.5));addParam(value);
            auto* algorithm=createParam<AlgorithmPicker>(mm2px(Vec(left+.55,24.3)),m,F::ALGO_A+i);algorithm->corner=i;addParam(algorithm);
        }
        scopes=new ScopePair;scopes->module=m;scopes->box.pos=mm2px(Vec(58,33));addChild(scopes);
        for(int i=0;i<12;++i) {
            float x=fractal::panel::controlX(i),y=fractal::panel::modulationY(i);
            auto* knob=createParamCentered<Dial>(mm2px(Vec(x,fractal::panel::mainY(i))),m,controls[i]);knob->color=controlColor(i);addParam(knob);
            auto* value=createParamCentered<ValueReadout>(mm2px(Vec(x,fractal::panel::readoutY(i))),m,controls[i]);value->control=i;value->color=controlColor(i);addParam(value);
            addParam(createParamCentered<FineDial>(mm2px(Vec(x-6,y)),m,attenuators[i]));
            addInput(createInputCentered<Jack>(mm2px(Vec(x+6,y)),m,inputs[i]));
        }
        addParam(createParam<BankPicker>(mm2px(Vec(12,118.5)),m,F::SLOT_PARAM));
        auto* store=createParam<TextSwitch>(mm2px(Vec(52.5,118.5)),m,F::STORE_PARAM);store->momentary=true;store->label="STORE";store->box.size=mm2px(Vec(16,7));addParam(store);
        auto* recall=createParam<TextSwitch>(mm2px(Vec(70,118.5)),m,F::RECALL_PARAM);recall->momentary=true;recall->label="RECALL";recall->box.size=mm2px(Vec(18,7));addParam(recall);
        addChild(createLightCentered<TinyLight<YellowLight>>(mm2px(Vec(94,122)),m,F::BUSY_LIGHT));
        addChild(createLightCentered<TinyLight<RedLight>>(mm2px(Vec(99,122)),m,F::ERROR_LIGHT));
        addOutput(createOutputCentered<OutputJack>(mm2px(Vec(129,122)),m,F::AUDIO_OUTPUT));
    }
    void onButton(const event::Button& e) override {
        if(e.button==GLFW_MOUSE_BUTTON_LEFT && e.pos.y>=px(8.4f)) {
            // Dispatch to the controls first. Empty body space has no drag target.
            widget::Widget::onButton(e);
            if(!e.isConsumed()) e.consume(nullptr);
            return;
        }
        ModuleWidget::onButton(e);
    }
    void appendContextMenu(Menu* menu) override {
        auto* m=dynamic_cast<F*>(module);if(!m)return;
        menu->addChild(new MenuSeparator);menu->addChild(createMenuLabel("Fractolian · complete tone memory"));
        int index=m->selectedSlot();menu->addChild(createMenuLabel(string::f("Name bank %02d",index+1)));
        auto* name=new BankNameField;name->index=index;name->box.size=Vec(250,25);name->text=m->bankName(index);name->placeholder="Enter a name";name->module=m;menu->addChild(name);
        menu->addChild(createMenuItem("Store current tone","",[m,index]{m->uiStore.store(index);}));
        menu->addChild(createMenuItem("Recall selected tone","",[m,index]{m->uiRecall.store(index);},!m->memories.occupied[index].load()));
        menu->addChild(createSubmenuItem("Select bank…","",[m](Menu* sub){addBankOptions(sub,m);}));
        menu->addChild(createMenuItem("Export all tones…","",[m]{exportTones(m);}));
        menu->addChild(createMenuItem("Import tone file…","",[m]{importTones(m);}));
        menu->addChild(new MenuSeparator);
        menu->addChild(createSubmenuItem("Position motion","",[m](Menu* sub){
            const char* names[]={"Scan","Bend","Crossfade"};
            for(int i=0;i<3;++i)sub->addChild(createCheckMenuItem(names[i],"",
                [m,i]{return int(m->params[F::POSITION_MODE_PARAM].getValue())==i;},
                [m,i]{auto* q=m->getParamQuantity(F::POSITION_MODE_PARAM);float old=q->getValue();
                    q->setValue(i);recordParam(m,F::POSITION_MODE_PARAM,old,"Change Position motion");}));
        }));
        menu->addChild(createSubmenuItem("Output voices",m->forcedChannels.load()?std::to_string(m->forcedChannels.load()):"Automatic",[m](Menu* sub){
            sub->addChild(createCheckMenuItem("Automatic from connected poly CV","",[m]{return m->forcedChannels.load()==0;},[m]{m->forcedChannels.store(0);}));
            for(int n=1;n<=16;++n)sub->addChild(createCheckMenuItem(std::to_string(n)+(n==1?" voice":" voices"),"",[m,n]{return m->forcedChannels.load()==n;},[m,n]{m->forcedChannels.store(n);}));
        }));
        menu->addChild(createMenuLabel("Output scope follows channel 1, including CV and filters."));
        menu->addChild(createMenuLabel("Click a readout for exact entry; Shift-drag for fine control."));
        menu->addChild(createMenuLabel("Roughness and source seeds are shared across voices."));
        menu->addChild(createMenuLabel("No internal LFO."));
    }
};
} // namespace
Model* modelFractalXY=createModel<FractalXY,FractalXYWidget>("FractalXY");
#endif // FRACTAL_HEADLESS_TEST
