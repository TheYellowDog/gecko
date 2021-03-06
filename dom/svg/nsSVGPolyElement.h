/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef NS_SVGPOLYELEMENT_H_
#define NS_SVGPOLYELEMENT_H_

#include "mozilla/Attributes.h"
#include "nsSVGPathGeometryElement.h"
#include "SVGAnimatedPointList.h"

typedef nsSVGPathGeometryElement nsSVGPolyElementBase;

namespace mozilla {
class DOMSVGPointList;
}

class nsSVGPolyElement : public nsSVGPolyElementBase
{
protected:
  explicit nsSVGPolyElement(already_AddRefed<mozilla::dom::NodeInfo>& aNodeInfo);

  virtual ~nsSVGPolyElement();

public:
  //interfaces

  NS_DECL_ISUPPORTS_INHERITED

  // nsIContent interface
  NS_IMETHOD_(bool) IsAttributeMapped(const nsIAtom* name) const MOZ_OVERRIDE;

  virtual SVGAnimatedPointList* GetAnimatedPointList() MOZ_OVERRIDE {
    return &mPoints;
  }
  virtual nsIAtom* GetPointListAttrName() const MOZ_OVERRIDE {
    return nsGkAtoms::points;
  }

  // nsSVGElement methods:
  virtual bool HasValidDimensions() const MOZ_OVERRIDE;

  // nsSVGPathGeometryElement methods:
  virtual bool AttributeDefinesGeometry(const nsIAtom *aName) MOZ_OVERRIDE;
  virtual bool IsMarkable() MOZ_OVERRIDE { return true; }
  virtual void GetMarkPoints(nsTArray<nsSVGMark> *aMarks) MOZ_OVERRIDE;
  virtual bool GetGeometryBounds(Rect* aBounds, const StrokeOptions& aStrokeOptions,
                                 const Matrix& aTransform) MOZ_OVERRIDE;

  // WebIDL
  already_AddRefed<mozilla::DOMSVGPointList> Points();
  already_AddRefed<mozilla::DOMSVGPointList> AnimatedPoints();

protected:
  SVGAnimatedPointList mPoints;
};

#endif //NS_SVGPOLYELEMENT_H_
