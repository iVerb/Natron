//  Natron
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RotoSmear.h"

#include "Engine/Node.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/RotoContext.h"

using namespace Natron;

struct RotoSmearPrivate
{
    QMutex smearDataMutex;
    std::pair<Point, double> lastTickPoint,lastCur;
    double lastDistToNext;
    
    RotoSmearPrivate()
    : smearDataMutex()
    , lastTickPoint()
    , lastCur()
    , lastDistToNext(0)
    {
        
    }
};

RotoSmear::RotoSmear(boost::shared_ptr<Natron::Node> node)
: EffectInstance(node)
, _imp(new RotoSmearPrivate())
{
    setSupportsRenderScaleMaybe(eSupportsYes);
}

RotoSmear::~RotoSmear()
{
    
}

void
RotoSmear::addAcceptedComponents(int /*inputNb*/,std::list<Natron::ImageComponents>* comps)
{
    comps->push_back(ImageComponents::getRGBAComponents());
    comps->push_back(ImageComponents::getRGBComponents());
    comps->push_back(ImageComponents::getXYComponents());
    comps->push_back(ImageComponents::getAlphaComponents());
}

void
RotoSmear::addSupportedBitDepth(std::list<Natron::ImageBitDepthEnum>* depths) const
{
    depths->push_back(Natron::eImageBitDepthFloat);
}

void
RotoSmear::getPreferredDepthAndComponents(int /*inputNb*/,std::list<Natron::ImageComponents>* comp,Natron::ImageBitDepthEnum* depth) const
{
    EffectInstance* input = getInput(0);
    if (input) {
        return input->getPreferredDepthAndComponents(-1, comp, depth);
    } else {
        comp->push_back(ImageComponents::getRGBAComponents());
        *depth = eImageBitDepthFloat;
    }
}


Natron::ImagePremultiplicationEnum
RotoSmear::getOutputPremultiplication() const
{
    EffectInstance* input = getInput(0);
    if (input) {
        return input->getOutputPremultiplication();
    } else {
        return eImagePremultiplicationPremultiplied;
    }
    
}

Natron::StatusEnum
RotoSmear::getRegionOfDefinition(U64 hash,SequenceTime time, const RenderScale & scale, int view, RectD* rod)
{
    (void)EffectInstance::getRegionOfDefinition(hash, time, scale, view, rod);
    
    RectD maskRod;
    boost::shared_ptr<Node> node = getNode();
    node->getPaintStrokeRoD(time, &maskRod);
    if (rod->isNull()) {
        *rod = maskRod;
    } else {
        rod->merge(maskRod);
    }
    return Natron::eStatusOK;
}

double
RotoSmear::getPreferredAspectRatio() const
{
    EffectInstance* input = getInput(0);
    if (input) {
        return input->getPreferredAspectRatio();
    } else {
        return 1.;
    }
    
}

bool
RotoSmear::isIdentity(SequenceTime time,
                const RenderScale & scale,
                const RectI & roi,
                int /*view*/,
                SequenceTime* inputTime,
                int* inputNb)
{
    RectD maskRod;
    boost::shared_ptr<Node> node = getNode();
    node->getPaintStrokeRoD(time, &maskRod);
    
    RectI maskPixelRod;
    maskRod.toPixelEnclosing(scale, getPreferredAspectRatio(), &maskPixelRod);
    if (!maskPixelRod.intersects(roi)) {
        *inputTime = time;
        *inputNb = 0;
        return true;
    }
    return false;
}

static ImagePtr renderSmearMaskDot(const boost::shared_ptr<RotoContext>& context, boost::shared_ptr<RotoStrokeItem>& stroke, const Point& p, double pressure, double brushSize, const ImageComponents& comps, ImageBitDepthEnum depth, unsigned int mipmapLevel)
{
    RectD dotRod(p.x - brushSize / 2., p.y - brushSize / 2., p.x + brushSize / 2., p.y + brushSize / 2.);
    
    std::list<std::pair<Point,double> > points;
    points.push_back(std::make_pair(p, pressure));
    ImagePtr ret;
    (void)context->renderSingleStroke(stroke, dotRod, points, mipmapLevel, 1., comps, depth, 0, &ret);
    return ret;
}


static void renderSmearDot(const boost::shared_ptr<RotoContext>& context,
                           boost::shared_ptr<RotoStrokeItem>& stroke,
                           const Point& prev,
                           const Point& next,
                           double /*prevPress*/,
                           double nextPress,
                           double brushSize,
                           ImageBitDepthEnum depth,
                           unsigned int mipmapLevel,
                           int nComps,
                           const ImagePtr& outputImage)
{
    ImagePtr dotMask = renderSmearMaskDot(context, stroke, next, nextPress, brushSize, ImageComponents::getAlphaComponents(), depth, mipmapLevel);
    assert(dotMask);
    
    RectI nextDotBounds = dotMask->getBounds();
    
    
    RectD prevDotRoD(prev.x - brushSize / 2., prev.y - brushSize / 2., prev.x + brushSize / 2., prev.y + brushSize / 2.);
    RectI prevDotBounds;
    prevDotRoD.toPixelEnclosing(mipmapLevel, outputImage->getPixelAspectRatio(), &prevDotBounds);
    
    //assert(prevDotBounds.width() == nextDotBounds.width() && prevDotBounds.height() == nextDotBounds.height());
    
    
    float fgPixels[4];
    
    ImagePtr tmpBuf(new Image(outputImage->getComponents(),prevDotRoD, prevDotBounds, mipmapLevel, outputImage->getPixelAspectRatio(), depth, false));
    tmpBuf->pasteFrom(*outputImage, prevDotBounds, false);
    
    Image::ReadAccess tmpAcc(tmpBuf.get());
    Image::WriteAccess wacc(outputImage.get());
    Image::ReadAccess mracc = dotMask->getReadRights();

    
    int yPrev = prevDotBounds.y1;
    for (int y = nextDotBounds.y1; y < nextDotBounds.y2; ++y,++yPrev) {
        
        float* dstPixels = (float*)wacc.pixelAt(nextDotBounds.x1, y);
        const float* maskPixels = (const float*)mracc.pixelAt(nextDotBounds.x1, y);
        const float* srcPixels = (const float*)tmpAcc.pixelAt(prevDotBounds.x1, yPrev);
        assert(srcPixels && dstPixels && maskPixels);
        
        int xPrev = prevDotBounds.x1;
        for (int x = nextDotBounds.x1; x < nextDotBounds.x2;
             ++x, ++xPrev,
             srcPixels += nComps,
             dstPixels += nComps,
             ++maskPixels) {
            
            
            for (int k = 0; k < nComps; ++k) {
                fgPixels[k] = srcPixels ? srcPixels[k] * *maskPixels : 0.;
            }
            
            for (int k = 0; k < nComps; ++k) {
                dstPixels[k] = fgPixels[k] + dstPixels[k] * (1. - *maskPixels);
            }

        }
    }
    

}

Natron::StatusEnum
RotoSmear::render(const RenderActionArgs& args)
{
    boost::shared_ptr<Node> node = getNode();
    boost::shared_ptr<RotoStrokeItem> stroke = node->getAttachedStrokeItem();
    boost::shared_ptr<RotoContext> context = stroke->getContext();
    assert(context);
    bool duringPainting = node->isDuringPaintStrokeCreation();
    bool isFirstStrokeTick = duringPainting && node->isFirstPaintStrokeRenderTick();
    
    unsigned int mipmapLevel = Image::getLevelFromScale(args.originalScale.x);
    
    std::list<std::pair<Natron::Point,double> > points;
    //stroke->evaluateStroke(0, &points);
    node->getLastPaintStrokePoints(args.time, &points);
    
    
    if (isFirstStrokeTick || !duringPainting) {
        QMutexLocker k(&_imp->smearDataMutex);
        _imp->lastCur.first.x = INT_MIN;
        _imp->lastCur.first.y = INT_MIN;
    }
    
    if ((int)points.size() <= 1) {
        return eStatusOK;
    }
    
    ComponentsNeededMap neededComps;
    bool processAll;
    bool processComponents[4];
    SequenceTime ptTime;
    int ptView;
    boost::shared_ptr<Node> ptInput;
    getComponentsNeededAndProduced_public(args.time, args.view, &neededComps, &processAll, &ptTime, &ptView, processComponents, &ptInput);
    
    ComponentsNeededMap::iterator foundBg = neededComps.find(0);
    assert(foundBg != neededComps.end() && !foundBg->second.empty());
    
    double par = getPreferredAspectRatio();
    RectI bgImgRoI;
    
    
    double brushSize = stroke->getBrushSizeKnob()->getValueAtTime(args.time);
    double brushSpacing = stroke->getBrushSpacingKnob()->getValueAtTime(args.time);
    if (brushSpacing > 0) {
        brushSpacing = std::max(0.05, brushSpacing);
    }
    
    brushSpacing = std::max(brushSpacing, 0.05);
        
    //This is the distance between each dot we render
    double maxDistPerSegment = brushSize * brushSpacing;
    
    double halfSize = maxDistPerSegment / 2.;

    
    double writeOnStart = stroke->getBrushVisiblePortionKnob()->getValueAtTime(args.time , 0);
    double writeOnEnd = stroke->getBrushVisiblePortionKnob()->getValueAtTime(args.time, 1);

    int firstPoint = (int)std::floor((points.size() * writeOnStart));
    int endPoint = (int)std::ceil((points.size() * writeOnEnd));
    assert(firstPoint >= 0 && firstPoint < (int)points.size() && endPoint > firstPoint && endPoint <= (int)points.size());
    
    std::list<std::pair<Point,double> > visiblePortion;
    std::list<std::pair<Point,double> >::const_iterator startingIt = points.begin();
    std::list<std::pair<Point,double> >::const_iterator endingIt = points.begin();
    std::advance(startingIt, firstPoint);
    std::advance(endingIt, endPoint);
    for (std::list<std::pair<Point,double> >::const_iterator it = startingIt; it!=endingIt; ++it) {
        visiblePortion.push_back(*it);
    }
 
    
    for (std::list<std::pair<Natron::ImageComponents,boost::shared_ptr<Natron::Image> > >::const_iterator plane = args.outputPlanes.begin();
         plane != args.outputPlanes.end(); ++plane) {
       
        
        int nComps = plane->first.getNumComponents();
        

        ImagePtr bgImg = getImage(0, args.time, args.mappedScale, args.view, 0, foundBg->second.front(), plane->second->getBitDepth(), par, false, &bgImgRoI);
        if (!bgImg) {
            plane->second->fill(args.roi, 0., 0., 0., 0.);
            continue;
        }
        
        //First copy the source image if this is the first stroke tick
        
        if (isFirstStrokeTick || !duringPainting) {
            plane->second->pasteFrom(*bgImg,args.roi, false);
        }
        
        if (brushSpacing == 0 || (writeOnEnd - writeOnStart) <= 0. || visiblePortion.empty()) {
            continue;
        }
        

        std::list<std::pair<Natron::Point,double> >::iterator it = visiblePortion.begin();
        
        //prev is the previously rendered point. On initialization this is just the point in the list prior to cur.
        //cur is the last point we rendered or the point before "it"
        //renderPoint is the final point we rendered, recorded for the next call to render when we are bulding up the smear
        std::pair<Point,double> lastCur,prev,cur,renderPoint;
        
        double distToNext = 0;

        
        {
            QMutexLocker k(&_imp->smearDataMutex);
            lastCur = _imp->lastCur;
        }
        
        if (isFirstStrokeTick || !duringPainting || (lastCur.first.x == INT_MIN && lastCur.first.y == INT_MIN)) {
            //This is the very first dot we render
            prev = *it;
            ++it;
            renderSmearDot(context,stroke,prev.first,it->first,prev.second,it->second,brushSize,plane->second->getBitDepth(),mipmapLevel, nComps, plane->second);
            renderPoint = *it;
            prev = renderPoint;
            ++it;
            if (it != visiblePortion.end()) {
                cur = *it;
            } else {
                cur = prev;
            }
        } else {
            QMutexLocker k(&_imp->smearDataMutex);
            prev = _imp->lastTickPoint;
            distToNext = _imp->lastDistToNext;
            renderPoint = prev;
            cur = _imp->lastCur;
        }
        
        
        while (it!=visiblePortion.end()) {
            
            
            //Render for each point a dot. Spacing is a percentage of brushSize:
            //Spacing at 1 means no dot is overlapping another (so the spacing is in fact brushSize)
            //Spacing at 0 we do not render the stroke
            
            double dx = it->first.x - cur.first.x;
            double dy = it->first.y - cur.first.y;
            double dist = std::sqrt(dx * dx + dy * dy);
            
            distToNext += dist;
            if (distToNext < maxDistPerSegment || dist == 0) {
                //We did not cross maxDistPerSegment pixels yet along the segments since we rendered cur, continue
                cur = *it;
                ++it;
                continue;
            }
            
            //Find next point by
            double a;
            if (maxDistPerSegment >= dist) {
                a = (distToNext - dist) == 0 ? (maxDistPerSegment - dist) / dist : (maxDistPerSegment - dist) / (distToNext - dist);
            } else {
                a = maxDistPerSegment / dist;
            }
            assert(a >= 0 && a <= 1);
            renderPoint.first.x = dx * a + cur.first.x;
            renderPoint.first.y = dy * a + cur.first.y;
            renderPoint.second = (it->second - cur.second) * a + cur.second;

            //prevPoint is the location of the center of the portion of the image we should copy to the renderPoint
            Point prevPoint;
            Point v;
            v.x = renderPoint.first.x - prev.first.x;
            v.y = renderPoint.first.y - prev.first.y;
            double vx = std::min(std::max(0. ,std::abs(v.x / halfSize)),.7);
            double vy = std::min(std::max(0. ,std::abs(v.y / halfSize)),.7);
            
            prevPoint.x = prev.first.x + vx * v.x;
            prevPoint.y = prev.first.y + vy * v.y;
            renderSmearDot(context,stroke,prevPoint,renderPoint.first,renderPoint.second,renderPoint.second,brushSize,plane->second->getBitDepth(),mipmapLevel, nComps,plane->second);
            
            prev = renderPoint;
            cur = renderPoint;
            distToNext = 0;
            
        }
        
        if (duringPainting) {
            QMutexLocker k(&_imp->smearDataMutex);
            _imp->lastTickPoint = prev;
            _imp->lastDistToNext = distToNext;
            _imp->lastCur = cur;
        }
    }
    node->updateLastPaintStrokeAge();
    return Natron::eStatusOK;
}