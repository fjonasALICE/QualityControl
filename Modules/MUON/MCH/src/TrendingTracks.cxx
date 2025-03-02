// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

///
/// \file    TrendingTracks.cxx
/// \author  Andrea Ferrero andrea.ferrero@cern.ch
/// \brief   Trending of the MCH tracking
/// \since   21/06/2022
///

#include "MCH/TrendingTracks.h"
#include "MUONCommon/MergeableTH2Ratio.h"
#include "MCHMappingInterface/Segmentation.h"
#include "MCHMappingSegContour/CathodeSegmentationContours.h"
#include "QualityControl/QcInfoLogger.h"
#include "QualityControl/DatabaseInterface.h"
#include "QualityControl/MonitorObject.h"
#include "QualityControl/Reductor.h"
#include "QualityControl/RootClassFactory.h"
#include <boost/property_tree/ptree.hpp>
#include <TH1.h>
#include <TMath.h>
#include <TH2.h>
#include <TCanvas.h>
#include <TPaveText.h>
#include <TDatime.h>
#include <TGraphErrors.h>
#include <TProfile.h>
#include <TPoint.h>

using namespace o2::quality_control;
using namespace o2::quality_control::core;
using namespace o2::quality_control::postprocessing;
using namespace o2::quality_control_modules::muon;
using namespace o2::quality_control_modules::muonchambers;

void TrendingTracks::configure(const boost::property_tree::ptree& config)
{
  mConfig = PostProcessingConfigMCH(getID(), config);
  for (int chamber = 0; chamber < 10; chamber++) {
    mConfig.plots.push_back({ fmt::format("Clusters_CH{}", chamber + 1),
                              fmt::format("Clusters CH{}", chamber + 1),
                              fmt::format("clusCH{}:time", chamber + 1),
                              "", "*L", "" });
  }
}

void TrendingTracks::initialize(Trigger, framework::ServiceRegistryRef)
{
  // Setting parameters

  // Preparing data structure of TTree
  mTrend = std::make_unique<TTree>();
  mTrend->SetName(PostProcessingInterface::getName().c_str());
  mTrend->Branch("runNumber", &mMetaData.runNumber);
  mTrend->Branch("time", &mTime);

  for (int chamber = 0; chamber < 10; chamber++) {
    mTrend->Branch(fmt::format("clusCH{}", chamber + 1).c_str(), &mClusCH[chamber]);
  }

  for (const auto& source : mConfig.dataSources) {
    std::unique_ptr<Reductor> reductor(root_class_factory::create<Reductor>(source.moduleName, source.reductorName));
    mTrend->Branch(source.name.c_str(), reductor->getBranchAddress(), reductor->getBranchLeafList());
    mReductors[source.name] = std::move(reductor);
  }
  getObjectsManager()->startPublishing(mTrend.get());
}

void TrendingTracks::computeClustersPerChamber(TProfile* p)
{
  if (!p) {
    return;
  }
  if (p->GetXaxis()->GetNbins() != 10) {
    ILOG(Error, Support) << "Wrong number of bins in moClusPerChamber, can't compute number of clusters per chamber";
    return;
  }

  if (!mHistClusPerChamberPrev) {
    mHistClusPerChamberPrev = new TProfile(*p);
    mHistClusPerChamberPrev->SetName("histClusPerChamberPrev");
    mHistClusPerChamberPrev->Reset();
  }

  TProfile hDiff(*p);
  hDiff.SetName("histClusPerChamberDiff");
  hDiff.Add(mHistClusPerChamberPrev, -1);

  for (int ch = 0; ch < 10; ch++) {
    mClusCH[ch] = hDiff.GetBinContent(ch + 1);
  }
}

// todo: see if OptimizeBaskets() indeed helps after some time
void TrendingTracks::update(Trigger t, framework::ServiceRegistryRef services)
{
  auto& qcdb = services.get<repository::DatabaseInterface>();

  trendValues(t, qcdb);
  generatePlots();
}

void TrendingTracks::finalize(Trigger t, framework::ServiceRegistryRef)
{
  generatePlots();
}

void TrendingTracks::trendValues(const Trigger& t, repository::DatabaseInterface& qcdb)
{
  mTime = t.timestamp / 1000; // ROOT expects seconds since epoch
  mMetaData.runNumber = t.activity.mId;

  std::shared_ptr<o2::quality_control::core::MonitorObject> moClusPerChamber = nullptr;

  for (auto& dataSource : mConfig.dataSources) {
    auto mo = qcdb.retrieveMO(dataSource.path, dataSource.name, t.timestamp, t.activity);
    if (!mo) {
      continue;
    }
    ILOG(Debug, Support) << "Got MO " << mo << ENDM;
    if (mo->getName().find(nameClusPerChamber) != std::string::npos) {
      moClusPerChamber = mo;
    } else {
      TObject* obj = mo ? mo->getObject() : nullptr;
      if (obj) {
        mReductors[dataSource.name]->update(obj);
      }
    }
  }

  if (moClusPerChamber) {
    TProfile* p = dynamic_cast<TProfile*>(moClusPerChamber->getObject());
    if (!p) {
      ILOG(Error, Support) << "Cannot cast moClusPerChamber, can't compute number of clusters per chamber";
      return;
    }
    computeClustersPerChamber(p);
  }

  mTrend->Fill();
}

void TrendingTracks::generatePlots()
{
  if (mTrend->GetEntries() < 1) {
    ILOG(Info, Support) << "No entries in the trend so far, won't generate any plots." << ENDM;
    return;
  }

  ILOG(Info, Support) << "Generating " << mConfig.plots.size() << " plots." << ENDM;

  for (const auto& plot : mConfig.plots) {

    // Before we generate any new plots, we have to delete existing under the same names.
    // It seems that ROOT cannot handle an existence of two canvases with a common name in the same process.
    if (mPlots.count(plot.name)) {
      getObjectsManager()->stopPublishing(plot.name);
      delete mPlots[plot.name];
    }

    // we determine the order of the plot, i.e. if it is a histogram (1), graph (2), or any higher dimension.
    const size_t plotOrder = std::count(plot.varexp.begin(), plot.varexp.end(), ':') + 1;
    // we have to delete the graph errors after the plot is saved, unfortunately the canvas does not take its ownership
    TGraphErrors* graphErrors = nullptr;

    TCanvas* c = new TCanvas();

    mTrend->Draw(plot.varexp.c_str(), plot.selection.c_str(), plot.option.c_str());

    c->SetName(plot.name.c_str());
    c->SetTitle(plot.title.c_str());

    // For graphs we allow to draw errors if they are specified.
    if (!plot.graphErrors.empty()) {
      if (plotOrder != 2) {
        ILOG(Error, Support) << "Non empty graphErrors seen for the plot '" << plot.name << "', which is not a graph, ignoring." << ENDM;
      } else {
        // We generate some 4-D points, where 2 dimensions represent graph points and 2 others are the error bars
        std::string varexpWithErrors(plot.varexp + ":" + plot.graphErrors);
        mTrend->Draw(varexpWithErrors.c_str(), plot.selection.c_str(), "goff");
        graphErrors = new TGraphErrors(mTrend->GetSelectedRows(), mTrend->GetVal(1), mTrend->GetVal(0), mTrend->GetVal(2), mTrend->GetVal(3));
        // We draw on the same plot as the main graph, but only error bars
        graphErrors->Draw("SAME E");
        // We try to convince ROOT to delete graphErrors together with the rest of the canvas.
        if (auto* pad = c->GetPad(0)) {
          if (auto* primitives = pad->GetListOfPrimitives()) {
            primitives->Add(graphErrors);
          }
        }
      }
    }

    // Postprocessing the plot - adding specified titles, configuring time-based plots, flushing buffers.
    // Notice that axes and title are drawn using a histogram, even in the case of graphs.
    if (auto histo = dynamic_cast<TH1*>(c->GetPrimitive("htemp"))) {
      // The title of histogram is printed, not the title of canvas => we set it as well.
      histo->SetTitle(plot.title.c_str());
      // We have to update the canvas to make the title appear.
      c->Update();

      // After the update, the title has a different size and it is not in the center anymore. We have to fix that.
      if (auto title = dynamic_cast<TPaveText*>(c->GetPrimitive("title"))) {
        title->SetBBoxCenterX(c->GetBBoxCenter().fX);
        // It will have an effect only after invoking Draw again.
        title->Draw();
      } else {
        ILOG(Error, Devel) << "Could not get the title TPaveText of the plot '" << plot.name << "'." << ENDM;
      }

      // We have to explicitly configure showing time on x axis.
      // I hope that looking for ":time" is enough here and someone doesn't come with an exotic use-case.
      if (plot.varexp.find(":time") != std::string::npos) {
        histo->GetXaxis()->SetTimeDisplay(1);
        // It deals with highly congested dates labels
        histo->GetXaxis()->SetNdivisions(505);
        // Without this it would show dates in order of 2044-12-18 on the day of 2019-12-19.
        histo->GetXaxis()->SetTimeOffset(0.0);
        histo->GetXaxis()->SetTimeFormat("%Y-%m-%d %H:%M");
      }
      // QCG doesn't empty the buffers before visualizing the plot, nor does ROOT when saving the file,
      // so we have to do it here.
      histo->BufferEmpty();
    } else {
      ILOG(Error, Devel) << "Could not get the htemp histogram of the plot '" << plot.name << "'." << ENDM;
    }

    mPlots[plot.name] = c;
    getObjectsManager()->startPublishing(c);
  }
}
